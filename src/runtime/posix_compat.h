/*
 * posix_compat.h - the Windows CRT names the runtime was written against,
 * answered by POSIX on macOS and Android.
 *
 * The handlers call _open, _stricmp, Sleep and friends; rather than fork every
 * call site into two, a POSIX host defines the names. Only what the runtime
 * uses is here.
 */
#ifndef POSIX_COMPAT_H
#define POSIX_COMPAT_H
#ifndef _WIN32

#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef uint32_t DWORD;

static inline void Sleep(DWORD ms)
{
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
}

#define _stricmp   strcasecmp
#define _strnicmp  strncasecmp
#define _strdup    strdup
#define _open      open
#define _close     close
#define _read      read
#define _write     write
#define _lseek     lseek
#define _unlink    unlink
#define _access    access
#define _chdir     chdir
#define _getcwd    getcwd
#define _mkdir(p)  mkdir((p), 0777)
#define _O_BINARY  0
#define _O_CREAT   O_CREAT
#define _O_TRUNC   O_TRUNC
#define _O_APPEND  O_APPEND

#define _stat64    stat

/* Win32 virtual-key codes: the input queue speaks them on every host. */
#define VK_ESCAPE 0x1B
#define VK_LEFT   0x25
#define VK_UP     0x26
#define VK_RIGHT  0x27
#define VK_DOWN   0x28
#define VK_F1     0x70
#define VK_F12    0x7B

static inline DWORD GetTickCount(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
#define localtime_s(out, t) (localtime_r((t), (out)) ? 0 : -1)

#endif
#endif
