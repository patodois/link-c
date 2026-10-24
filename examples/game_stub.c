/* Stands in for a game's frame loop so that host_example.c and guest_example.c run: one "frame"
 * every 16 ms for 3 seconds. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* nanosleep, before the first system header */
#endif
#include "../rewrite_link.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

int game_running(void) {
    static uint64_t start;
    if (start == 0) start = rbl_now_us();
#if defined(_WIN32)
    Sleep(16);
#else
    { struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 16 * 1000000L; nanosleep(&ts, NULL); }
#endif
    return rbl_now_us() - start < 3000000u;
}
