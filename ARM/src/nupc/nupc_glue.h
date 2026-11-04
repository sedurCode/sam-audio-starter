/*
 * C interface to the NUPC convolution engine for the SAM audio starter (ARM core 0).
 *
 *   nupc_start()              once at start-up, before audio runs (allocates; not real-time safe)
 *   nupc_process_s32()        from the audio path (the DMA callback) for every block - never blocks
 *   nupc_service_background() from a low-priority task / the idle loop - runs queued background FFT steps
 *
 * The engine itself is C++ (neural::arm::NupcConvolver); see docs/NUPC-THREADING-PLAN.md.
 */
#ifndef NUPC_GLUE_H
#define NUPC_GLUE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sampleRate;      /* Hz, 48000 */
    uint32_t cpuHz;           /* core clock, for the cycle-counter clock (CCLK) */
    uint32_t irLength;        /* samples of the embedded impulse response to use, 0 = all */
    uint32_t minBlock;        /* first (direct, zero-latency) stage, power of two, normally == audio block (64) */
    uint32_t maxBlock;        /* largest FFT partition, power of two */
    uint32_t foregroundFrames;/* stages with a deadline below this run on the audio callback */
    bool     backgroundTier;  /* false: everything on the audio callback (no background task needed) */
} NUPC_CONFIG;

typedef struct {
    bool     active;
    uint32_t irLength, numStages, workingKiB, arenaUsedKiB, arenaCapacityKiB;
    uint32_t blocks;             /* audio blocks processed */
    uint32_t cyclesLast, cyclesMax, cyclesAvg;  /* nupc_process_s32() cost in CPU cycles */
    uint32_t budgetCycles;       /* one audio block's worth of CPU cycles */
    uint32_t overBudget;         /* blocks that took longer than the budget */
    uint32_t backgroundSteps, foregroundSteps, stolenSteps, misses, restarts, maxQueued;
} NUPC_STATS;

/* Fills a config with the harness defaults (1 s of IR, 64-sample head, 4096 max partition, background tier on). */
void nupc_default_config(NUPC_CONFIG *cfg);

/* Returns 0 on success, negative on failure (bad config, arena too small). */
int  nupc_start(const NUPC_CONFIG *cfg);
void nupc_stop(void);

/* Convolve one channel of an interleaved int32 audio block in place (full-scale int32 <-> +-1.0). Passes the audio
 * through untouched if the engine is not running or numFrames does not match the engine's block size. */
void nupc_process_s32(int32_t *interleaved, unsigned numChannels, unsigned numFrames, unsigned channel);

/* Runs up to maxSteps queued background steps; returns how many ran (0 = nothing queued). */
unsigned nupc_service_background(unsigned maxSteps);
bool     nupc_background_idle(void);

void nupc_set_enabled(bool enabled);

/* On-target correctness check: feeds an impulse through the engine and compares the output with the impulse response
 * itself (the engine's output for an impulse IS the scaled IR). Runs twice: with the background serviced between
 * blocks and with it never serviced (every step stolen by the callback). Exercises the real Ne10 NEON FFT.
 * Only legal while the engine is disabled (nupc_set_enabled(false)). Returns 0 on success, negative if it could not run. */
typedef struct {
    float    peak;            /* largest |expected| sample */
    float    maxErrServiced;  /* max |out - expected| with background servicing */
    float    maxErrStolen;    /* max |out - expected| with every step stolen */
    uint32_t samples;         /* samples compared per pass */
    uint32_t backgroundSteps, stolenSteps;
} NUPC_SELFTEST;
int nupc_selftest(NUPC_SELFTEST *result);
void nupc_get_stats(NUPC_STATS *stats);
void nupc_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* NUPC_GLUE_H */
