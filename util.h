//
// Created on 10/30/17.
//

#ifndef SOCKNM_UTIL_H
#define SOCKNM_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
// caution!! this may occur error if not declared __cplusplus when compiling with c++ project

int64_t MAX(int64_t i1, int64_t i2);

int64_t MIN(int64_t i1, int64_t i2);

uint32_t iclock();

#ifdef __cplusplus
}
#endif

#endif //SOCKNM_UTIL_H
