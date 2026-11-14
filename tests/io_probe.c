/* Test-only syscall interposition. Never linked into the database image. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>
#include <time.h>

static uint64_t database_read_bytes;
static uint64_t response_flush_failures;
static uint64_t database_sync_calls;
static uint64_t database_sync_nanoseconds;
static uint64_t database_write_calls;
static uint64_t database_write_bytes;
static int fd_has_suffix(int fd, const char* suffix);
static void commit_stop(int fd, const char* stage);
static uint64_t catalog_allocation_failures;
static int fail_next_allocation;
extern void* __libc_malloc(size_t);
void* malloc(size_t size) {
    if (fail_next_allocation) {
        fail_next_allocation = 0;
        ++catalog_allocation_failures;
        errno = ENOMEM;
        return NULL;
    }
    return __libc_malloc(size);
}
static void arm_catalog_failure(int fd, uint64_t offset, ssize_t result) {
    const char* selected = getenv("MVD_FAIL_CATALOG_OFFSET");
    if (selected && !catalog_allocation_failures && result > 2 &&
        offset == strtoull(selected, NULL, 10) && fd_has_suffix(fd, "/database.mvd"))
        fail_next_allocation = 1;
}
static int fd_has_suffix(int fd, const char* suffix) {
    char path[4096], link[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof path - 1);
    if (n <= 0) return 0;
    path[n] = 0;
    size_t length = (size_t)n;
    size_t suffix_size = strlen(suffix);
    return length >= suffix_size && strcmp(path + length - suffix_size, suffix) == 0;
}
ssize_t pread(int fd, void* out, size_t n, off_t offset) {
    static ssize_t (*real_pread)(int, void*, size_t, off_t);
    if (!real_pread) real_pread = dlsym(RTLD_NEXT, "pread");
    ssize_t result = real_pread(fd, out, n, offset);
    int saved = errno;
    if (result > 0 && fd_has_suffix(fd, "/database.mvd")) database_read_bytes += (uint64_t)result;
    arm_catalog_failure(fd, (uint64_t)offset, result);
    errno = saved;
    return result;
}
ssize_t pread64(int fd, void* out, size_t n, off64_t offset) {
    static ssize_t (*real_pread64)(int, void*, size_t, off64_t);
    if (!real_pread64) real_pread64 = dlsym(RTLD_NEXT, "pread64");
    ssize_t result = real_pread64(fd, out, n, offset);
    int saved = errno;
    if (result > 0 && fd_has_suffix(fd, "/database.mvd")) database_read_bytes += (uint64_t)result;
    arm_catalog_failure(fd, (uint64_t)offset, result);
    errno = saved;
    return result;
}
static int fail_response_flush(int fd, size_t n) {
    if (getenv("MVD_FAIL_RESPONSE_FLUSH") && n && n < 8192 && fd_has_suffix(fd, "/response.tmp")) {
        ++response_flush_failures;
        errno = ENOSPC;
        return 1;
    }
    return 0;
}
ssize_t pwrite(int fd, const void* data, size_t n, off_t offset) {
    static ssize_t (*real_pwrite)(int, const void*, size_t, off_t);
    if (!real_pwrite) real_pwrite = dlsym(RTLD_NEXT, "pwrite");
    if (offset < 8192 && n == 4096) commit_stop(fd, "before_root");
    ssize_t result = real_pwrite(fd, data, n, offset); int saved = errno;
    if (result > 0 && fd_has_suffix(fd, "/database.mvd")) { ++database_write_calls; database_write_bytes += result; }
    if (offset < 8192 && n == 4096 && result == (ssize_t)n) commit_stop(fd, "after_root");
    errno = saved; return result;
}
ssize_t pwrite64(int fd, const void* data, size_t n, off64_t offset) {
    static ssize_t (*real_pwrite64)(int, const void*, size_t, off64_t);
    if (!real_pwrite64) real_pwrite64 = dlsym(RTLD_NEXT, "pwrite64");
    if (offset < 8192 && n == 4096) commit_stop(fd, "before_root");
    ssize_t result = real_pwrite64(fd, data, n, offset); int saved = errno;
    if (result > 0 && fd_has_suffix(fd, "/database.mvd")) { ++database_write_calls; database_write_bytes += result; }
    if (offset < 8192 && n == 4096 && result == (ssize_t)n) commit_stop(fd, "after_root");
    errno = saved; return result;
}
static uint64_t nanoseconds(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
int fdatasync(int fd) {
    static int (*real_sync)(int); if (!real_sync) real_sync = dlsym(RTLD_NEXT, "fdatasync");
    commit_stop(fd, "before_sync"); uint64_t start = nanoseconds();
    int result = real_sync(fd), saved = errno;
    if (!result && fd_has_suffix(fd, "/database.mvd")) { ++database_sync_calls; database_sync_nanoseconds += nanoseconds() - start; }
    if (!result) commit_stop(fd, "after_sync"); errno = saved; return result;
}
int fsync(int fd) {
    static int (*real_sync)(int); if (!real_sync) real_sync = dlsym(RTLD_NEXT, "fsync");
    commit_stop(fd, "before_sync"); uint64_t start = nanoseconds();
    int result = real_sync(fd), saved = errno;
    if (!result && fd_has_suffix(fd, "/database.mvd")) { ++database_sync_calls; database_sync_nanoseconds += nanoseconds() - start; }
    if (!result) commit_stop(fd, "after_sync"); errno = saved; return result;
}
ssize_t write(int fd, const void* data, size_t n) {
    static ssize_t (*real_write)(int, const void*, size_t);
    if (!real_write) real_write = dlsym(RTLD_NEXT, "write");
    if (fail_response_flush(fd, n)) return -1;
    return real_write(fd, data, n);
}
ssize_t writev(int fd, const struct iovec* data, int count) {
    static ssize_t (*real_writev)(int, const struct iovec*, int);
    if (!real_writev) real_writev = dlsym(RTLD_NEXT, "writev");
    size_t n = 0;
    for (int i = 0; i < count; ++i) n += data[i].iov_len;
    if (fail_response_flush(fd, n)) return -1;
    return real_writev(fd, data, count);
}
static void stop_at(const char* stage) {
    const char* selected = getenv("MVD_COMPACT_STOP");
    const char* marker = getenv("MVD_COMPACT_MARKER");
    if (!selected || !marker || strcmp(selected, stage) != 0) return;
    int fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) _exit(121);
    if (write(fd, stage, strlen(stage)) < 0) _exit(122);
    close(fd);
    raise(SIGSTOP);
}
static void commit_stop(int fd, const char* stage) {
    const char* selected = getenv("MVD_COMMIT_STOP"), *marker = getenv("MVD_COMMIT_MARKER"), *armed = getenv("MVD_COMMIT_ARM");
    if (!selected || !marker || !armed || strcmp(selected, stage) || access(armed, F_OK) || !fd_has_suffix(fd, "/database.mvd")) return;
    int signal_fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (signal_fd < 0 || write(signal_fd, stage, strlen(stage)) < 0) _exit(125);
    close(signal_fd); raise(SIGSTOP);
}
static void recovery_stop(int fd, const char* stage) {
    const char* selected = getenv("MVD_RECOVERY_STOP");
    const char* marker = getenv("MVD_RECOVERY_MARKER");
    if (!selected || !marker || strcmp(selected, stage) || !fd_has_suffix(fd, "/database.mvd")) return;
    int signal_fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (signal_fd < 0) _exit(123);
    if (write(signal_fd, stage, strlen(stage)) < 0) _exit(124);
    close(signal_fd);
    raise(SIGSTOP);
}
int ftruncate(int fd, off_t length) {
    static int (*real_ftruncate)(int, off_t);
    if (!real_ftruncate) real_ftruncate = dlsym(RTLD_NEXT, "ftruncate");
    recovery_stop(fd, "before_truncate");
    int result = real_ftruncate(fd, length);
    int saved = errno;
    if (!result) recovery_stop(fd, "after_truncate");
    errno = saved;
    return result;
}
int ftruncate64(int fd, off64_t length) {
    static int (*real_ftruncate64)(int, off64_t);
    if (!real_ftruncate64) real_ftruncate64 = dlsym(RTLD_NEXT, "ftruncate64");
    recovery_stop(fd, "before_truncate");
    int result = real_ftruncate64(fd, length);
    int saved = errno;
    if (!result) recovery_stop(fd, "after_truncate");
    errno = saved;
    return result;
}
int rename(const char* from, const char* to) {
    static int (*real_rename)(const char*, const char*);
    if (!real_rename) real_rename = dlsym(RTLD_NEXT, "rename");
    int compact = strstr(from, "/.compact-work/database.mvd") != NULL;
    if (compact) stop_at("before_replace");
    int result = real_rename(from, to);
    int saved = errno;
    if (compact && result == 0) stop_at("after_replace");
    errno = saved;
    return result;
}
__attribute__((destructor)) static void report_reads(void) {
    const char* path = getenv("MVD_IO_REPORT");
    if (!path) return;
    FILE* file = fopen(path, "w");
    if (!file) return;
    fprintf(file, "{\"database_read_bytes\":%llu,\"response_flush_failures\":%llu,\"catalog_allocation_failures\":%llu,\"database_write_calls\":%llu,\"database_write_bytes\":%llu,\"database_sync_calls\":%llu,\"database_sync_nanoseconds\":%llu}\n",
            (unsigned long long)database_read_bytes, (unsigned long long)response_flush_failures,
            (unsigned long long)catalog_allocation_failures, (unsigned long long)database_write_calls,
            (unsigned long long)database_write_bytes, (unsigned long long)database_sync_calls, (unsigned long long)database_sync_nanoseconds);
    fclose(file);
}
