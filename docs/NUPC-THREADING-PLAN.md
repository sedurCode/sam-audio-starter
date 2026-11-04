# Plan: NUPC on the SAM (ADSP-SC589) ARM core - threading strategy and integration

Revision 3 (2026-11-04). Integration implemented (steps 1-5 below); verification on hardware still open.

## Context
`../nupc/` (outside this repo) holds the non-uniform partitioned convolution (NUPC) engine from i.MX8/i.MX95
Linux. There it uses `ThreadedTaskScheduler` (std::thread pool, POSIX semaphore, mutex, condvar).
The **commercial product is bare-metal with no threads**. This repo is the **test harness**: the SAM
audio starter (FreeRTOS on the Cortex-A5, SAE IPC to two SHARC+ cores), built with CMake.

**Decision:** keep the engine core untouched and implement one new `TaskScheduler`
(`neural::arm::TaskScheduler`, `nupc/Convolution.h:26-45`) that has two *drivers* for background work:

| Build | Foreground (audio callback) | Background driver |
|---|---|---|
| Harness (this repo, FreeRTOS) | `processAudio()` path runs head + small stages | one low-priority FreeRTOS task calls `nupc::serviceBackground()` |
| Product (bare metal) | audio ISR runs head + small stages | idle `main()` loop calls `nupc::serviceBackground()` |

The scheduler code is identical; only the thing that calls `serviceBackground()` differs, so the harness
exercises exactly the code the product ships. FreeRTOS full-adoption in the product was rejected (it takes
over IRQ dispatch, tick and stacks).

## Facts established so far
- Hardware: ADSP-SC589, Cortex-A5 (NEON/VFPv4, hard-float) + 2 SHARC+. (The older SC594 workspace was moved to `../SC594/`.)
- Toolchains: CCES **2.8.3** (GCC 4.8.3, C++98 default -> need `-std=gnu++11` for NUPC) and **2.11.1**
  (GCC 10.2, C++14 default). The starter builds on both through CMake (`README.md`).
- ARM audio entry point: `processAudio()` in `ARM/src/process_audio.c:191`, called from the codec/A2B/SPDIF DMA callbacks.
  `SYSTEM_BLOCK_SIZE = 64`, `SYSTEM_SAMPLE_RATE = 48000` (`ARM/src/context.h:56-57`) - NUPC's
  `BUFFER_SIZE_SAMPLES` default of 32 must be set to 64 (or the glue must re-block).
- FreeRTOS config (`ARM/include/FreeRTOSConfig.h`): preemptive, 1 kHz tick, 7 priorities, **256 KB heap**
  - far too small for NUPC buffers (about 16 B/tap, ~8 MB for a 10 s IR): use a dedicated static arena in DDR/L2, not the FreeRTOS heap.
- CCES IRQ dispatcher (`.../cortex-a5/runtime/Source/int/exception_dispatcher.S`, same in 2.8.3 and 2.11.1) saves
  FPSCR/FPEXC and D0-D7/D16-D31 around handlers, so NEON in an ISR is safe against NEON in thread context.
  **Resolved:** `configUSE_TASK_FPU_SUPPORT = 2`, so FreeRTOS saves the VFP/NEON context for every task, and the CA5 dispatcher is FPU-safe in ISRs.
- nupc sources are OS-free below `NupcConvolver`; the bare-metal seam is `TaskScheduler`. Ne10 is the FFT (now vendored);
  upstream `ne10_init()` reads `/proc/cpuinfo`, replaced by `ne10_bm_init()`.
- ADI SC589 has a hardware FFT accelerator (`adi_fft.h`): possible future `RealFft` backend.

## Steps
### Done
- SC589 project: SAM audio starter + CMake for all three cores; debug/release on CCES 2.8.3 and 2.11.1.
- VS Code debug profile for the open SHARC+ toolchain (`.vscode/`, see README).
- **1. nupc imported** into `ARM/src/nupc/` (C++11, `-fno-exceptions -fno-rtti`), guarded by `NUPC_BARE_METAL`; the
  Linux code path and the engine core are unchanged. Details: `ARM/src/nupc/README.md`.
- **2. Ne10 vendored** (float32 FFT only, NEON intrinsics, no assembly) in `ARM/src/oss-services/ne10/`: no
  `/proc/cpuinfo`, allocations routed to the NUPC arena, built with `NE10_DSP_RFFT_SCALING` so c2r applies 1/N as
  `Convolution.h` expects (without that define neither direction scales).
- **3. `NupcPlatform`**: PMCCNTR clock (core clock = `CCLK`, 450 MHz on the SAM), flush-to-zero, arena (16 MB in DDR,
  `SAM_NUPC_ARENA_MB`).
- **4. `BareMetalTaskScheduler`**: foreground tier on the callback + EDF background tier via `serviceBackground()`;
  the FreeRTOS harness runs it from `nupcBackgroundTask` (priority idle+1).
- **5. Starter integration**: `nupc_process_s32()` is called from `processAudio()` on codec-in channel 0 (in place,
  before routing); `nupc_start()` runs once in `startupTask` before the clock domains start; the engine starts
  **disabled** - use the shell: `nupc on|off|reset|test`. The 16 KB supervisor/interrupt stack replaces the 1 KB
  default (`init.c`) and lives in DDR via `build/ARM-SAM-Audio-Starter-nupc.ld` (L2 has no room).
- Host test (`test/host/`): engine vs direct convolution, 4 scheduler modes, TSAN clean.

### Open
- **Run on hardware**: `nupc test` (real Ne10 NEON FFT), then `nupc on` with audio and watch `nupc` stats.
- Deadline misses under load; tune `foregroundFrames` (512), `kBackgroundBudgetFraction` (0.70), IR length (1 s), `maxBlock` (4096).
- Cost of the scalar head FIR and MAC loops (no NEON intrinsics; GCC does not auto-vectorise them at -O1/-O2).
- Stereo / more channels; real IR loading instead of the embedded York Minster table.
- Bare-metal product: drive `nupc_service_background()` from the idle loop instead of a FreeRTOS task.

### Verification checklist
1. Builds on both CCES versions (`debug`, `release`, `debug-283`, `release-283`) - done for debug, debug-283, release.
2. Host test passes (`test/host`) - done.
3. `nupc test` PASS on target (max error < 1e-3 of the IR peak, serviced and stolen).
4. 60 s real-time run with `nupc on`: zero deadline misses; `cycles/block max` below the budget (64 samples = 1.33 ms = 600 k cycles at 450 MHz).
5. Raise IR length / `maxBlock` until misses appear: the sustainable reverb length, and whether finer priority levels
   (GIC software interrupts, SST style) are needed.
6. Product fit: integration surface is `nupc_start()` at init, `nupc_process_s32()` in the audio path, `nupc_service_background()` from an idle context.

## Debugging
Open SHARC+ toolchain (WSL `~/git/sharc-plus-toolchain`): `lldb-dap` + patched OpenOCD + ICE-1000. It debugs
Cortex-A5 and both SHARC+ cores, including compound sessions. NUPC runs on the A5, so use the "Cortex-A5 SC589" profiles.
See `README.md` > Debugging.
