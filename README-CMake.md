# CMake build for the SAM (ADZS-SC589-MINI) audio starter

CMake port of `build/makefile` (kept untouched). Builds all three cores and the loader images:

| Target          | Output                                             |
|-----------------|----------------------------------------------------|
| `sam_arm`       | `SAM-Audio-Starter-ARM.exe` (Cortex-A5, GCC)       |
| `sam_sharc0`    | `SAM-Audio-Starter-SHARC0.dxe` (cc21k)             |
| `sam_sharc1`    | `SAM-Audio-Starter-SHARC1.dxe` (cc21k)             |
| `sam_ldr`       | `SAM-Audio-Starter.ldr` (default target)           |
| `sam_full_ldr`  | `SAM-Audio-Starter-Full.ldr` (boot0+boot1+app; needs sh/cat/truncate, i.e. Git bash) |

```
cmake --preset release            # or: debug
cmake --build --preset release
cmake --build out/release --target sam_full_ldr
```

Outputs land in `out/<preset>/`. Debug = `-g`; Release = `-O1` (ARM) / `-O1 -Ov100 -Oa` (SHARC), same as the makefile.

## Choosing the CCES version
Presets `debug`/`release` use CCES **2.11.1** (the version ADI verified); `debug-283`/`release-283`
use **2.8.3**. Override with `-DCCES_ROOT=...` or the `CCES_ROOT` environment variable. With neither,
the toolchain file uses `arm-none-eabi-gcc` from PATH (`cc21k`/`elfloader` must also be on PATH).

CCES 2.8.3 builds this starter via compatibility shims, applied only when GCC < 5 (see `CMakeLists.txt`):
- `-std=gnu99` (GCC 4.8 defaults to gnu90; FatFs exFAT and many `for (int ...)` loops need C99).
- `cmake/compat/sru.h` - 2.8.3's ARM tree has no generic `<sru.h>`, only `sruSC589.h`.
- `cmake/compat/sam_compat_endian.h` (force-included) - 2.8.3's newlib lacks `__htons`/`__ntohs`/`__htonl`/`__ntohl`.

Only the 2.11.1 build was checked beyond "links and makes an LDR"; neither build has been run on hardware.

## Layout
- `CMakeLists.txt`, `CMakePresets.json`, `cmake/cces-arm-toolchain.cmake` - the CMake port.
- `build/` - linker scripts (`.ld`, `.ldf`), init code, bootloader LDRs (used by CMake) and the original makefile.
- `ARM/`, `SHARC0/`, `SHARC1/`, `ALL/` - sources, as in the upstream starter.
