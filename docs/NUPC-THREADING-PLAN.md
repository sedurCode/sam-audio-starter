# Plan: NUPC on the SAM (ADSP-SC589) ARM core - threading strategy and integration

Revision 2 (2026-11-04). Supersedes the first draft: the project now exists (SAM audio starter, CMake),
and the harness uses FreeRTOS.

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
  **Open item:** confirm the FreeRTOS CA5 port (`portable/CCES/ARM_CA5`) saves the NEON/VFP context per task, otherwise a
  NEON-using background task can corrupt another task. Check before enabling the background task.
- nupc sources are OS-free below `NupcConvolver`; the bare-metal seam is `TaskScheduler`. Ne10 is the FFT and is **not**
  in this repo yet. `ne10_init()` reads `/proc/cpuinfo` and must be replaced.
- ADI SC589 has a hardware FFT accelerator (`adi_fft.h`): possible future `RealFft` backend.

## Steps
### Done
- SC589 project created: SAM audio starter (fork `sedurCode/sam-audio-starter`) + CMake port for all three cores; debug
  and release build on CCES 2.8.3 and 2.11.1 (`CMakeLists.txt`, `CMakePresets.json`, `cmake/`).
- VS Code debug profile for the open SHARC+ toolchain's LLDB/OpenOCD (`.vscode/`, see README).

### 1. Bring nupc into the repo
- Add `nupc/` under `ARM/src/nupc/` (or as a linked folder), compiled with `-std=gnu++11`; add C++ language to CMake
  for the ARM target only. The starter is C-only today, so check `-fno-exceptions -fno-rtti` and libstdc++ linking.
- Define `NUPC_BARE_METAL`; compile out `ThreadedTaskScheduler.hpp`, `<thread>/<mutex>/<chrono>`, `TraceMarker`.

### 2. Vendor Ne10
- Build only the real-FFT parts for armv7-a NEON hard-float; replace `ne10_init()`; route `NE10_MALLOC` to the arena.
- Settle inverse-FFT scaling with an on-target round-trip test (`fft.hpp` and `Convolution.h:254-258` disagree).

### 3. `NupcPlatform.h`
- PMCCNTR cycle counter (`readCycles`), `readNanos()` = cycles / measured `cpuHz` (replaces `steady_clock`).
- ARMv7 flush-to-zero (FPSCR bit 24) in the thread/ISR contexts that run NUPC.

### 4. `BareMetalTaskScheduler.hpp` (core work)
- `start(task, deadlineFrames)` from the audio callback: tasks with deadline `< fgThresholdFrames` run in the callback
  (reuse `AudioThreadTaskScheduler` round-robin + time budget); others are queued with an absolute due time.
- `serviceBackground(maxSteps)`: pick earliest-deadline queued task, claim by CAS, run **one step** (one FFT, one MAC chunk
  or one IFFT), re-queue or mark done. Stepping lets EDF re-pick, bounding blocking to one FFT.
- `join()` in the callback: done -> consume; queued -> steal and run inline; running (background was preempted mid-step)
  -> count a deadline miss and drop that block. 32-bit atomics only (lock-free on Cortex-A5).
- Harness driver: one FreeRTOS task, lowest priority above idle, loops `serviceBackground()`. Product driver: idle loop.
- Stats: misses, steals, queue depth, worst callback cycles, background busy/idle cycles.

### 5. Integration in the starter
- Add a NUPC processing stage reachable from `processAudio()` (new stream/route in `route.h`, or a codec-in -> NUPC -> codec-out
  inline stage); mono first.
- Keep the arena in DDR (or L2 for short IRs); do not use the FreeRTOS heap.

### 6. Verification
1. Builds on both CCES versions (`debug`, `release`, `debug-283`, `release-283`), no `pthread`/`sem_*` symbols (`arm-none-eabi-nm`).
2. FFT round-trip error < 1e-5 on target.
3. Measured `cpuHz` printed (expect ~450-500 MHz).
4. 60 s real-time run: zero deadline misses; worst callback cycles as % of the 64-sample budget (1.33 ms).
5. Output vs offline reference (`AudioThreadTaskScheduler`, all joined inline): max abs diff <= 1e-6 when misses = 0.
6. Raise IR length / max block until misses appear: the sustainable reverb length, and whether finer priority levels
   (GIC software interrupts, SST style) are needed.
7. Product fit: integration surface is `prepare()` at init, `process()` in the audio path, `serviceBackground()` from an idle context.

## Debugging
Open SHARC+ toolchain (WSL `~/git/sharc-plus-toolchain`): `lldb-dap` + patched OpenOCD + ICE-1000. It debugs
Cortex-A5 and both SHARC+ cores, including compound sessions. NUPC runs on the A5, so use the "Cortex-A5 SC589" profiles.
See `README.md` > Debugging.
