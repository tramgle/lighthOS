#ifndef _LIMITS_H
#define _LIMITS_H

#define CHAR_BIT    8
#define SCHAR_MAX   127
#define SCHAR_MIN   (-128)
#define UCHAR_MAX   255
#define CHAR_MAX    SCHAR_MAX
#define CHAR_MIN    SCHAR_MIN

#define SHRT_MAX    32767
#define SHRT_MIN    (-32768)
#define USHRT_MAX   65535

#define INT_MAX     2147483647
#define INT_MIN     (-INT_MAX - 1)
#define UINT_MAX    0xffffffffU

#define LONG_MAX    INT_MAX
#define LONG_MIN    INT_MIN
#define ULONG_MAX   UINT_MAX

#define LLONG_MAX   0x7fffffffffffffffLL
#define LLONG_MIN   (-LLONG_MAX - 1)
#define ULLONG_MAX  0xffffffffffffffffULL

#define MB_LEN_MAX  1

#endif
