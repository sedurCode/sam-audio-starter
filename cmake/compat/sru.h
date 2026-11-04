/* Compatibility shim for CCES < 2.9: the ARM tree has no generic <sru.h>, only the
 * per-processor header. Newer CCES versions ship their own <sru.h> that dispatches
 * to the same file, so this shim is only added to the include path for old toolchains. */
#ifndef SAM_COMPAT_SRU_H
#define SAM_COMPAT_SRU_H
#include <sruSC589.h>
#endif
