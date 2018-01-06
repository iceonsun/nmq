//
// Created on 10/30/17.
//

#ifndef SOCKNM_UTIL_H
#define SOCKNM_UTIL_H

#include "ktype.h"

#ifdef __cplusplus
extern "C" {
#endif
// caution!! this may occur error if not declared __cplusplus when compiling with c++ project

IINT64 MAX(IINT64 i1, IINT64 i2);

IINT64 MIN(IINT64 i1, IINT64 i2);

IUINT32 iclock();

#ifdef __cplusplus
}
#endif

#endif //SOCKNM_UTIL_H
