// Test-only compatibility: preserve libc bounds checking and route valid
// fortified reads through the test's existing pread fault/counting wrapper.
#include <sys/types.h>
#include <unistd.h>

#if defined(__GLIBC__)
extern "C" ssize_t __wrap_pread(int,void*,size_t,off_t);
extern "C" ssize_t __real___pread_chk(int,void*,size_t,off_t,size_t);
extern "C" ssize_t __wrap___pread_chk(int fd,void* buffer,size_t bytes,
                                       off_t offset,size_t buffer_size) {
    if(bytes>buffer_size)
        return __real___pread_chk(fd,buffer,bytes,offset,buffer_size);
    return __wrap_pread(fd,buffer,bytes,offset);
}
#endif
