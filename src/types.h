#ifndef __TYPES_INCLUDED_
#define __TYPES_INCLUDED_ 1
 
#ifdef __MVS__
 
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed int         int32_t;
 
#ifndef NULL
#define NULL 0
#endif
 
#else
 
#include <stdint.h>
#include <stddef.h>
 
#endif
 
#endif
