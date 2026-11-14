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

static uint64_t database_read_bytes;
static uint64_t response_flush_failures;
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
    errno = saved;
    return result;
}
ssize_t pread64(int fd, void* out, size_t n, off64_t offset) {
    static ssize_t (*real_pread64)(int, void*, size_t, off64_t);
    if (!real_pread64) real_pread64 = dlsym(RTLD_NEXT, "pread64");
    ssize_t result = real_pread64(fd, out, n, offset);
    int saved = errno;
    if (result > 0 && fd_has_suffix(fd, "/database.mvd")) database_read_bytes += (uint64_t)result;
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
    fprintf(file, "{\"database_read_bytes\":%llu,\"response_flush_failures\":%llu}\n",
            (unsigned long long)database_read_bytes, (unsigned long long)response_flush_failures);
    fclose(file);
}
