# NUPC on the SAM (ADSP-SC589) - test harness

Test harness for running a non-uniform partitioned convolution (NUPC) engine on the Cortex-A5 of the
SHARC Audio Module (ADZS-SC589-MINI). It is ADI's SAM audio starter (FreeRTOS on the ARM, two SHARC+
cores) with a CMake build and a VS Code debug profile for the open SHARC+ toolchain. The NUPC code itself
is integrated (builds, host-tested, not yet run on the board) - see [docs/NUPC-THREADING-PLAN.md](docs/NUPC-THREADING-PLAN.md).

The original ADI README is kept as [README-SAM-Audio-Starter.md](README-SAM-Audio-Starter.md); the original
makefile build in `build/` still works.

## Layout
| Path | Contents |
|---|---|
| `ARM/` | Cortex-A5 (core 0) application: FreeRTOS, lwIP, drivers, `processAudio()` |
| `SHARC0/`, `SHARC1/` | SHARC+ core 1 / core 2 applications |
| `ALL/` | Code shared by all cores (SAE inter-core IPC) |
| `build/` | Linker scripts (`.ld`/`.ldf`), init code, bootloader LDRs, original makefile |
| `CMakeLists.txt`, `CMakePresets.json`, `cmake/` | CMake build (details in [README-CMake.md](README-CMake.md)) |
| `.vscode/` | Debug profile for the WSL sharc-plus-toolchain (below) |
| `docs/` | Plan and design notes |

## NUPC convolution
The convolution engine (`ARM/src/nupc/`, FFT: vendored Ne10 in `ARM/src/oss-services/ne10/`) is built into the ARM
application by default (CMake option `SAM_NUPC`, arena size `SAM_NUPC_ARENA_MB`). It processes **codec input channel 0**
(mono, 32-sample blocks, 48 kHz) in `processAudio()` before the routing table, with the embedded 10 s York Minster IR
(1 s used by default). It is prepared at boot but **disabled**; from the shell (USB CDC / UART):

```
nupc test     # impulse self-test through the real Ne10 NEON FFT (run this first)
nupc on       # enable;  nupc off to bypass
nupc          # statistics: cycles per block vs budget, background/foreground/stolen steps, deadline misses
```
Threading (no OS threads in the engine): small partitions run on the audio callback, large ones as queued steps that a
low-priority FreeRTOS task executes (the product would call the same `nupc_service_background()` from its idle loop).
See [ARM/src/nupc/README.md](ARM/src/nupc/README.md) and [docs/NUPC-THREADING-PLAN.md](docs/NUPC-THREADING-PLAN.md).

Host unit test (native, in WSL; no hardware): `cmake -S test/host -B /tmp/nupc-host -DCMAKE_BUILD_TYPE=Release && cmake --build /tmp/nupc-host && /tmp/nupc-host/test_nupc`
(`-DNUPC_TSAN=ON` for ThreadSanitizer; `NUPC_PACE_US` sets the concurrent mode's callback pacing, 1333 = real time).

**Status:** builds on CCES 2.8.3 and 2.11.1; host test passes. Not yet run on the board.

## Build
Needs CMake >= 3.20, Ninja, and CCES 2.8.3 or 2.11.1 under `C:\Analog Devices\`. From Git bash or any shell:

```
cmake --preset debug-283 && cmake --build --preset debug-283      # CCES 2.8.3
cmake --preset debug      && cmake --build --preset debug         # CCES 2.11.1
cmake --build out/debug --target sam_full_ldr                      # optional: boot0+boot1+app flash image
```
Presets: `debug`, `release` (2.11.1) and `debug-283`, `release-283` (2.8.3). Outputs are in `out/<preset>/`:
`SAM-Audio-Starter-ARM.exe`, `-SHARC0.dxe`, `-SHARC1.dxe`, `SAM-Audio-Starter.ldr` (and `-Full.ldr`).
ARM is a native CMake target; the SHARC cores use `cc21k` through custom commands.
CCES 2.8.3 builds via small compatibility shims in `cmake/compat/` (see README-CMake.md).

## Debugging with the open SHARC+ toolchain (WSL, LLDB-DAP)
Uses `~/git/sharc-plus-toolchain` in WSL: its `lldb-dap`, patched OpenOCD and an ADI ICE-1000. It can debug
the Cortex-A5 and both SHARC+ cores, singly or as a compound session.

**One-time setup**
1. Build the toolchain's debug tools in WSL (see its README): `target/release/sharc-debug`, `sharc-lldb-dap`,
   `target/llvm-build/bin/lldb-dap`, `target/openocd-build/src/openocd`.
2. Install the ICE-1000 udev rule in WSL and attach the probe to WSL from Windows (`usbipd attach --wsl ...`).
3. In VS Code: install **WSL** (Remote), `llvm-vs-code-extensions.lldb-dap`, and optionally the toolchain's
   `sharc-debug-tools-0.1.0.vsix`.
4. Open this folder through WSL: `code /mnt/d/SEDAudio/NUPC-ARM/SC589` from a WSL shell (or *WSL: Open Folder in WSL*).

**Profiles** (Run and Debug): `Cortex-A5 SC589 (ICE-1000, attach)`, `SHARC+ SC589 core 1/2 (ICE-1000, load and debug)`,
and compounds *SHARC+ cores 1 + 2* and *SC589 A5 + SHARC+ cores 1 + 2*. Build a `debug` preset first.

**Configuration** lives in `.vscode/settings.json`:
- `sharc-plus.toolchain` - toolchain checkout in WSL (`/home/sedur/git/sharc-plus-toolchain`).
- `sharc-plus.debugProgramA5` / `debugProgramCore1` / `debugProgramCore2` - default to `out/debug/...` (CCES 2.11.1 build).
  Point them at `out/debug-283/...` to debug the 2.8.3 build.
- Launch/tasks run the toolchain binaries by absolute path; session and profile state goes to `out/`.
- `sourceMap` in `launch.json` maps the `D:\SEDAudio\NUPC-ARM\SC589` paths the CCES compilers record in DWARF to the WSL workspace.

**Status:** verified offline that the toolchain's LLDB reads our SHARC DXEs (symbols, line tables) and that
`sharc-debug validate-a5-symbols` accepts the ARM `.exe`. **Not yet tried against hardware.**
The profiles were derived from the toolchain's own `.vscode/` files, minus the simulator.
Note: the WSL distro that holds the toolchain is `Ubuntu-26.04` (not 24.04).

CLion is not configured; the same `lldb-dap` binary can be used from it via a custom DAP/LLDB toolchain if wanted.
