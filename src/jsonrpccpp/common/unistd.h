// unistd.h - Windows portability shim for the libjson-rpc-cpp stream helpers.
// On MSVC (_WIN32) there is no POSIX <unistd.h>, so this maps the POSIX read()/
// write() calls used by StreamReader/StreamWriter onto the CRT's _read()/
// _write() (from <io.h>) and defines ssize_t via the Win32 SSIZE_T type. On
// non-Windows builds it simply includes the real <unistd.h>. This is part of
// what makes the RPC transport compile in this Windows fork.
#ifndef UNISTD_H
#define UNISTD_H

#ifdef _WIN32
#include <io.h>
#include <BaseTsd.h>

#define read _read
#define write _write

typedef SSIZE_T ssize_t;
#else // !_WIN32
#include <unistd.h>
#endif

#endif // !UNISTD_H
