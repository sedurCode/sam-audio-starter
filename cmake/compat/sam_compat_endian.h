/* Force-included for old CCES (GCC < 5) only. Its newlib machine/endian.h lacks the
 * __htons/__ntohs/__htonl/__ntohl macros newer newlib provides. Cortex-A5 is little-endian. */
#ifndef SAM_COMPAT_ENDIAN_H
#define SAM_COMPAT_ENDIAN_H
#ifndef __htons
#define __htons(x) __builtin_bswap16(x)
#define __ntohs(x) __builtin_bswap16(x)
#define __htonl(x) __builtin_bswap32(x)
#define __ntohl(x) __builtin_bswap32(x)
#endif
#endif
