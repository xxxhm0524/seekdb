#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int database_fd(int fd) {
    char link[64], path[4096]; snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t size = readlink(link, path, sizeof(path) - 1);
    if (size < 0) return 0; path[size] = 0;
    const char* tail = strrchr(path, '/'); return tail && strcmp(tail + 1, "database.mvd") == 0;
}
static void note(const char* path, const char* text) {
    if (!path) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) {
        size_t length = strlen(text), done = 0;
        while (done < length) {
            ssize_t n = write(fd, text + done, length - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            done += (size_t)n;
        }
        close(fd);
    }
}
static int wrapped(int fd, int mode, off_t offset, off_t length, const char* symbol) {
    typedef int (*function)(int, int, off_t, off_t);
    function real = (function)dlsym(RTLD_NEXT, symbol);
    if (!real) { errno = ENOSYS; return -1; }
    const char* arm = getenv("MVD_RECLAIM_ARM");
    const char* inject = getenv("MVD_RECLAIM_INJECT");
    int active = arm && inject && access(arm, F_OK) == 0 && database_fd(fd);
    if (active) {
        note(getenv("MVD_RECLAIM_COUNTER"), "x");
        if (strcmp(inject, "enosys") == 0) { errno = ENOSYS; return -1; }
        if (strcmp(inject, "eio") == 0) { errno = EIO; return -1; }
        if (strcmp(inject, "before") == 0) { note(getenv("MVD_RECLAIM_MARKER"), "before"); raise(SIGSTOP); }
    }
    int result = real(fd, mode, offset, length);
    if (active && strcmp(inject, "after") == 0) { note(getenv("MVD_RECLAIM_MARKER"), "after"); raise(SIGSTOP); }
    return result;
}
int fallocate(int fd, int mode, off_t offset, off_t length) { return wrapped(fd, mode, offset, length, "fallocate"); }
int fallocate64(int fd, int mode, off64_t offset, off64_t length) { return wrapped(fd, mode, offset, length, "fallocate64"); }
