//
// Created on 10/30/17.
//

#include <sys/time.h>
#include "util.h"

IINT64 MAX(IINT64 i1, IINT64 i2) {
    return i1 > i2 ? i1 : i2;
}

IINT64 MIN(IINT64 i1, IINT64 i2) {
    return i1 < i2 ? i1 : i2;
}

/* get system time */
static void itimeofday(long *sec, long *usec) {
    struct timeval time;
    gettimeofday(&time, 0);
    if (sec) *sec = time.tv_sec;
    if (usec) *usec = time.tv_usec; // microseconds. 1 micro_sec = 10e-6 sec
}

/* get clock in millisecond 64 */
static IINT64 iclock64(void) {
    long s, u;
    IINT64 value;
    itimeofday(&s, &u);
    value = ((IINT64) s) * 1000 + (u / 1000);
    return value;
}

// get clock in millisecond
IUINT32 iclock() {
    return (IUINT32) (iclock64() & 0xfffffffful);
}