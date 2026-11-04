// C glue between the SAM audio starter and the NUPC engine. See nupc_glue.h.
#include "nupc_glue.h"

#ifndef NUPC_BARE_METAL
#error "nupc_glue.cpp is the bare-metal/SAM integration; build it with -DNUPC_BARE_METAL"
#endif

#include <new>
#include <string.h>
#include <math.h>
#include "NeuralNonUniformPartitionedConvolution.h"

extern "C" ne10_result_t ne10_bm_init(void); // ARM/src/oss-services/ne10/ne10_bm_init.c

namespace {

alignas(16) unsigned char g_storage[sizeof(NupcConvolver)];
NupcConvolver* g_engine = 0;
volatile bool g_enabled = false;
float g_sampleRate = 48000.0f;

// Float staging buffers for one block (stack is only 4 KB in some contexts, keep them static).
float g_block[BUFFER_SIZE_SAMPLES];

// Per-block cost accounting (written by the audio callback, read by anyone: plain 32-bit counters).
volatile uint32_t g_blocks = 0, g_cyclesLast = 0, g_cyclesMax = 0, g_overBudget = 0;
volatile uint64_t g_cyclesSum = 0;
uint32_t g_budgetCycles = 0;

}  // namespace

extern "C" {

void nupc_default_config(NUPC_CONFIG* cfg)
{
    cfg->sampleRate = 48000;
    cfg->cpuHz = 500000000u;
    cfg->irLength = 48000;          // 1 s: fits comfortably even if everything stays on the callback
    cfg->minBlock = BUFFER_SIZE_SAMPLES;
    cfg->maxBlock = 4096;
    cfg->foregroundFrames = 512;
    cfg->backgroundTier = true;
}

int nupc_start(const NUPC_CONFIG* cfg)
{
    nupc_stop();

    nupc_clock_init(cfg->cpuHz);
    nupc_enable_flush_to_zero();
    if (ne10_bm_init() != NE10_OK)
        return -1;

    g_sampleRate = static_cast<float>(cfg->sampleRate);
    g_budgetCycles = static_cast<uint32_t>(
        static_cast<uint64_t>(cfg->cpuHz) * BUFFER_SIZE_SAMPLES / (cfg->sampleRate ? cfg->sampleRate : 48000));

    g_engine = new (g_storage) NupcConvolver();

    NupcConvolver::Config c;
    c.irLength = cfg->irLength;
    c.minBlock = cfg->minBlock;
    c.maxBlock = cfg->maxBlock;
    c.minTaskBlock = 2 * cfg->minBlock;
    c.foregroundFrames = cfg->foregroundFrames;
    c.scheduler = cfg->backgroundTier ? NupcConvolver::kSchedulerThreaded : NupcConvolver::kSchedulerAudioThread;
    if (!g_engine->prepare(g_sampleRate, c))
    {
        g_engine->~NupcConvolver();
        g_engine = 0;
        return -2;
    }
    nupc_reset_stats();
    g_enabled = true;
    return 0;
}

void nupc_stop(void)
{
    g_enabled = false;     // the callback now passes audio through
    if (g_engine)
    {
        g_engine->release();
        g_engine->~NupcConvolver();
        g_engine = 0;
    }
}

void nupc_set_enabled(bool enabled) { g_enabled = enabled; }

void nupc_process_s32(int32_t* interleaved, unsigned numChannels, unsigned numFrames, unsigned channel)
{
    NupcConvolver* const e = g_engine;
    if (!g_enabled || e == 0 || numFrames != BUFFER_SIZE_SAMPLES || channel >= numChannels)
        return;

    const uint32_t t0 = nupc_read_cycles();

    const float toFloat = 1.0f / 2147483648.0f;
    int32_t* p = interleaved + channel;
    for (unsigned i = 0; i < BUFFER_SIZE_SAMPLES; ++i, p += numChannels)
        g_block[i] = static_cast<float>(*p) * toFloat;

    e->process(g_block);

    p = interleaved + channel;
    for (unsigned i = 0; i < BUFFER_SIZE_SAMPLES; ++i, p += numChannels)
    {
        float v = g_block[i] * 2147483648.0f;
        if (v > 2147483520.0f) v = 2147483520.0f;
        if (v < -2147483648.0f) v = -2147483648.0f;
        *p = static_cast<int32_t>(v);
    }

    const uint32_t dt = nupc_read_cycles() - t0;
    g_cyclesLast = dt;
    if (dt > g_cyclesMax) g_cyclesMax = dt;
    if (dt > g_budgetCycles) ++g_overBudget;
    g_cyclesSum += dt;
    ++g_blocks;
}

unsigned nupc_service_background(unsigned maxSteps)
{
    NupcConvolver* const e = g_engine;
    return (g_enabled && e) ? e->serviceBackground(maxSteps) : 0;
}

bool nupc_background_idle(void)
{
    NupcConvolver* const e = g_engine;
    return e ? e->backgroundIdle() : true;
}

void nupc_reset_stats(void)
{
    g_blocks = 0;
    g_cyclesLast = 0;
    g_cyclesMax = 0;
    g_overBudget = 0;
    g_cyclesSum = 0;
    if (g_engine)
        g_engine->resetCounters();
}

void nupc_get_stats(NUPC_STATS* s)
{
    memset(s, 0, sizeof(*s));
    NupcConvolver* const e = g_engine;
    s->arenaUsedKiB = static_cast<uint32_t>(nupc_arena_used() / 1024);
    s->arenaCapacityKiB = static_cast<uint32_t>(nupc_arena_capacity() / 1024);
    s->blocks = g_blocks;
    s->cyclesLast = g_cyclesLast;
    s->cyclesMax = g_cyclesMax;
    s->cyclesAvg = g_blocks ? static_cast<uint32_t>(g_cyclesSum / g_blocks) : 0;
    s->budgetCycles = g_budgetCycles;
    s->overBudget = g_overBudget;
    if (!e)
        return;
    const NupcConvolver::Stats st = e->stats();
    s->active = st.active && g_enabled;
    s->irLength = static_cast<uint32_t>(st.irLength);
    s->numStages = static_cast<uint32_t>(st.numStages);
    s->workingKiB = static_cast<uint32_t>(st.workingFloats * sizeof(float) / 1024);
    const neural::arm::detail::BareMetalTaskScheduler::Counters c = e->schedulerCounters();
    s->backgroundSteps = c.backgroundSteps;
    s->foregroundSteps = c.foregroundSteps;
    s->stolenSteps = c.stolenSteps;
    s->misses = c.misses;
    s->restarts = c.restarts;
    s->maxQueued = c.maxQueued;
}

}  // extern "C"

// ---- On-target self-test ------------------------------------------------------------------------------------------
namespace {

// One pass: impulse in, compare engine output with scale * IR. `service` = steps of serviceBackground() after each block.
float selftestPass(NupcConvolver* e, float scale, unsigned blocks, unsigned service)
{
    float block[BUFFER_SIZE_SAMPLES];
    float maxErr = 0.0f;
    e->reset();
    for (unsigned b = 0; b < blocks; ++b)
    {
        for (unsigned i = 0; i < BUFFER_SIZE_SAMPLES; ++i)
            block[i] = (b == 0 && i == 0) ? 1.0f : 0.0f;
        e->process(block);
        for (unsigned i = 0; i < BUFFER_SIZE_SAMPLES; ++i)
        {
            const uint32_t bits = york_minster_ir_bits[b * BUFFER_SIZE_SAMPLES + i];
            float ir;
            memcpy(&ir, &bits, sizeof(ir));
            const float err = fabsf(block[i] - scale * ir);
            if (err > maxErr) maxErr = err;
        }
        if (service)
            e->serviceBackground(service);
    }
    return maxErr;
}

}  // namespace

extern "C" int nupc_selftest(NUPC_SELFTEST* r)
{
    NupcConvolver* const e = g_engine;
    memset(r, 0, sizeof(*r));
    if (!e || g_enabled)
        return -3;
    const NupcConvolver::Stats st = e->stats();
    unsigned samples = static_cast<unsigned>(st.irLength < 16384 ? st.irLength : 16384);
    const unsigned blocks = samples / BUFFER_SIZE_SAMPLES;
    if (blocks == 0)
        return -4;
    samples = blocks * BUFFER_SIZE_SAMPLES;

    const float scale = st.irScale;
    for (unsigned i = 0; i < samples; ++i)
    {
        const uint32_t bits = york_minster_ir_bits[i];
        float ir;
        memcpy(&ir, &bits, sizeof(ir));
        const float a = fabsf(scale * ir);
        if (a > r->peak) r->peak = a;
    }

    e->resetCounters();
    r->maxErrServiced = selftestPass(e, scale, blocks, 1);
    const neural::arm::detail::BareMetalTaskScheduler::Counters c1 = e->schedulerCounters();
    r->backgroundSteps = c1.backgroundSteps;
    r->maxErrStolen = selftestPass(e, scale, blocks, 0);
    const neural::arm::detail::BareMetalTaskScheduler::Counters c2 = e->schedulerCounters();
    r->stolenSteps = c2.stolenSteps - c1.stolenSteps;
    r->samples = samples;
    e->reset();
    e->resetCounters();
    return 0;
}
