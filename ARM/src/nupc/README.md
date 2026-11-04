# NUPC engine on the SAM ARM core

Non-uniform partitioned convolution (Gardner layout) imported from the Monolith sources
(`D:\SEDAudio\NUPC-ARM\nupc\`, i.MX8/i.MX95 Linux). The engine core (`Convolution.h`, `fft.hpp`) is **unchanged**;
everything below `NupcConvolver` is OS-free and talks to its scheduling environment through
`neural::arm::TaskScheduler`.

## What was added or changed for bare metal (`NUPC_BARE_METAL`, set by the CMake build)

| File | Change |
|---|---|
| `BareMetalTaskScheduler.hpp/.cpp` | **New.** Foreground/background `TaskScheduler`, no threads, nothing blocks (below). |
| `NupcPlatform.h/.cpp` | **New.** Arena allocator (replaces `new[]`/`malloc`), PMU cycle-counter clock (replaces `steady_clock`), flush-to-zero. Has host fallbacks so the unit test builds natively. |
| `nupc_glue.h/.cpp` | **New.** C API used by the starter: `nupc_start`, `nupc_process_s32`, `nupc_service_background`, stats, self-test. |
| `shell_nupc.c` | **New.** `nupc [on\|off\|reset\|test]` shell command. |
| `NeuralNonUniformPartitionedConvolution.h/.cpp` | `#ifdef NUPC_BARE_METAL` around the thread/mutex/`<chrono>` pieces; arena instead of `new[]`; `NupcHost` (control-thread double buffering) is not built; new `Config::foregroundFrames`; `serviceBackground()`/`schedulerCounters()` accessors. The Linux code path is untouched. |
| `york_minster_ir.h` | The 1.92 MB IR table is defined once (`NUPC_DEFINE_IR_TABLE` in the `.cpp`); other includers get an `extern` declaration. Otherwise a `static` copy per translation unit would double the image. |

Ne10 (the FFT) is vendored separately in `../oss-services/ne10/` (see its `MODIFICATIONS.md`).

## Threading model

`BareMetalTaskScheduler` splits the engine's tasks (each FFT stage is a task of steps: forward FFT, up to 8 MAC
chunks, inverse FFT) by deadline:

* **foreground** (deadline < `foregroundFrames`, default 512 frames): small stages run on the audio callback within a
  time budget (`advance()`), as `AudioThreadTaskScheduler` does.
* **background** (everything else): `start()` queues the task; `serviceBackground()` runs **one step** of the queued task
  with the earliest deadline. In this harness a FreeRTOS task at `tskIDLE_PRIORITY+1` calls it
  (`nupcBackgroundTask`, `ARM/src/main.c`); the bare-metal product calls the same function from its idle loop.
* `join()` on the callback finishes whatever is left (counted as *stolen* steps), so output is always correct unless the
  background was preempted *inside* a step of a task that is due now (a *deadline miss*: one block of that stage is late,
  counted in the stats). The window is one step wide; a background context that keeps up never hits it.

Single core, preemption only: 32-bit lock-free atomics, no locks, no waiting in the callback.

## Verification

* `test/host/` (native build, run in WSL): output equals a double-precision direct convolution to 4e-7 for the
  audio-thread, steal-everything, inline-serviced and **concurrent background thread** modes; clean under
  ThreadSanitizer at real-time pacing. It uses a reference FFT shim, so it validates the engine and schedulers, not Ne10.
* On target: `nupc test` runs an impulse through the engine with the real Ne10 NEON FFT and compares with the IR.
  **Not yet run on hardware.**
