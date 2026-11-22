// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// Device or host test: the Darwin errno slot must keep a failure that happens after
// the program cleared errno (the P/Invoke SetLastError pattern).

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "macho.h"

int main(void)
{
    int *(*error)(void) = (int *(*)(void))shim_lookup("___error", NULL);
    int failures = 0;
    errno = ENOENT;
    *error() = 0;                       // program clears errno before the call
    if (link("/nonexistent/a", "/nonexistent/b") == 0) return 2;
    int host = errno;
    int seen = *error();                // program reads errno after the call
    if (seen != errno_to_apple(host)) { failures++; printf("FAIL lost errno: host %d, program sees %d\n", host, seen); }
    *error() = 35;                      // program stores EAGAIN (Darwin 35)
    if (*error() != 35) { failures++; printf("FAIL program write lost\n"); }
    *error() = 0;                       // cleared, then a call succeeds without touching errno
    getpid();
    if (*error() != 0) { failures++; printf("FAIL successful call reported an error\n"); }
    *error() = 0;                       // cleared, then a call fails with a different error
    if (rmdir("/") == 0) return 2;
    if (*error() == 0) { failures++; printf("FAIL second failure lost\n"); }
    int fds[2];                         // would-block reads must stay EAGAIN, not Darwin EDEADLK (11)
    char byte;
    if (pipe(fds) != 0 || fcntl(fds[0], F_SETFL, O_NONBLOCK) != 0) return 2;
    *error() = 0;
    if (read(fds[0], &byte, 1) != -1) return 2;
    if (*error() != 35) { failures++; printf("FAIL EAGAIN reached the program as %d\n", *error()); }
    if (errno_to_apple(EDEADLK) != 11 || errno_from_apple(11) != EDEADLK || errno_from_apple(35) != EAGAIN) {
        failures++;
        printf("FAIL EAGAIN/EDEADLK mapping\n");
    }
    printf(failures ? "%d FAILURES\n" : "errno mirror ok (host %d -> darwin %d)\n", failures, host, seen);
    return failures != 0;
}
