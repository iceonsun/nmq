//
// Created on 10/30/17.
//

#include <sys/time.h>
#include "util.h"

int64_t MAX(int64_t i1, int64_t i2) {
    return i1 > i2 ? i1 : i2;
}

int64_t MIN(int64_t i1, int64_t i2) {
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
static int64_t iclock64(void) {
    long s, u;
    int64_t value;
    itimeofday(&s, &u);
    value = ((int64_t) s) * 1000 + (u / 1000);
    return value;
}

// get clock in millisecond
uint32_t iclock() {
    return (uint32_t) (iclock64() & 0xfffffffful);
}