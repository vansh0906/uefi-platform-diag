/* Host-build stand-in for EDK2's Base.h: just the types and macros DiagCore.c uses. */
#ifndef HOST_SHIM_BASE_H_
#define HOST_SHIM_BASE_H_
#include <stddef.h>
#include <stdint.h>
typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef uintptr_t UINTN;
typedef unsigned char BOOLEAN;
typedef char      CHAR8;
typedef void      VOID;
#define CONST  const
#define STATIC static
#define IN
#define OUT
#define OPTIONAL
#define EFIAPI
#define TRUE  ((BOOLEAN)1)
#define FALSE ((BOOLEAN)0)
#define BIT0 0x01
#define BIT1 0x02
#define BIT2 0x04
#define BIT3 0x08
#define BIT4 0x10
#define BIT5 0x20
#define BIT6 0x40
#define BIT7 0x80
#endif
