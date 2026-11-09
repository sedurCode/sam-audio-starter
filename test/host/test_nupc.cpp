// Host test for the NUPC engine with the bare-metal schedulers (build + run: see CMakeLists.txt).
//
// Checks, for each scheduler mode, that the engine's output equals a direct (double precision) convolution of the
// same input with the same impulse-response prefix, scaled the way NupcConvolver scales it. Modes:
//   audio     : AudioThreadTaskScheduler, everything on the "callback"        (reference behaviour)
//   bm-steal  : BareMetalTaskScheduler, nothing ever services the background -> join() steals every step
//   bm-inline : BareMetalTaskScheduler, serviceBackground() called between callbacks from the same thread
//   bm-thread : BareMetalTaskScheduler, a second thread runs serviceBackground() concurrently with the "callback"
//               (real concurrency; stronger than the single-core preemption it models)
#include "NeuralNonUniformPartitionedConvolution.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

static const std::size_t kIrLength = 8192;
static const std::size_t kSamples = 32 * 800;   // 25600 samples, 800 callbacks

struct Result { double maxErr; neural::arm::detail::BareMetalTaskScheduler::Counters c; };

static std::vector<float> loadIr(std::size_t n)
{
    std::vector<float> ir(n);
    neural::arm::detail::loadIrSamplePrefix(york_minster_ir_bits, YORK_MINSTER_IR_LENGTH, ir.data(), n);
    return ir;
}

static std::vector<double> directConv(const std::vector<float>& ir, const std::vector<float>& in)
{
    std::vector<double> out(in.size(), 0.0);
    for (std::size_t n = 0; n < in.size(); ++n)
    {
        double acc = 0.0;
        const std::size_t kmax = n < ir.size() - 1 ? n : ir.size() - 1;
        for (std::size_t k = 0; k <= kmax; ++k)
            acc += static_cast<double>(ir[k]) * in[n - k];
        out[n] = acc;
    }
    return out;
}

enum Mode { kAudio, kBmSteal, kBmInline, kBmThread };

// Callback pacing for bm-thread in microseconds (real time at 48 kHz/32 samples is 667). NUPC_PACE_US overrides.
static unsigned paceUs() { const char* e = std::getenv("NUPC_PACE_US"); return e ? static_cast<unsigned>(std::atoi(e)) : 160u; }

static Result run(Mode mode, const std::vector<float>& in, const std::vector<double>& ref)
{
    NupcConvolver* e = new NupcConvolver();
    NupcConvolver::Config c;
    c.irLength = kIrLength;
    c.minBlock = BUFFER_SIZE_SAMPLES;
    c.maxBlock = 2048;
    c.minTaskBlock = 2 * BUFFER_SIZE_SAMPLES;
    c.foregroundFrames = 512;
    c.scheduler = mode == kAudio ? NupcConvolver::kSchedulerAudioThread : NupcConvolver::kSchedulerThreaded;
    if (!e->prepare(48000.0f, c)) { std::printf("prepare failed\n"); std::exit(2); }
    const float scale = e->stats().irScale;

    std::atomic<bool> stop(false);
    std::thread bg;
    if (mode == kBmThread)
        bg = std::thread([&] { while (!stop.load()) { if (e->serviceBackground(1) == 0) std::this_thread::yield(); } });

    std::vector<float> out(in.size());
    for (std::size_t b = 0; b < in.size(); b += BUFFER_SIZE_SAMPLES)
    {
        e->process(&in[b], &out[b]);
        if (mode == kBmInline && (b / BUFFER_SIZE_SAMPLES) % 2 == 1)
            e->serviceBackground(2);
        if (mode == kBmThread)   // pace the callback (default ~8x real time) so the background thread can keep up
            std::this_thread::sleep_for(std::chrono::microseconds(paceUs()));
    }
    stop = true;
    if (bg.joinable()) bg.join();

    Result r;
    r.maxErr = 0.0;
    for (std::size_t n = 0; n < in.size(); ++n)
    {
        const double d = std::fabs(static_cast<double>(out[n]) - scale * ref[n]);
        if (d > r.maxErr) r.maxErr = d;
    }
    if (mode != kAudio) r.c = e->schedulerCounters(); else std::memset(&r.c, 0, sizeof(r.c));
    delete e;
    return r;
}

int main()
{
    nupc_clock_init(500000000u);
    nupc_arena_init(std::malloc(64u << 20), 64u << 20);

    const std::vector<float> ir = loadIr(kIrLength);
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> in(kSamples);
    for (auto& s : in) s = dist(rng);
    const std::vector<double> ref = directConv(ir, in);

    const char* names[] = { "audio", "bm-steal", "bm-inline", "bm-thread" };
    int failures = 0;
    for (int m = 0; m < 4; ++m)
    {
        const Result r = run(static_cast<Mode>(m), in, ref);
        const bool exact = r.maxErr < 1e-4;
        // A deadline miss legitimately corrupts that block (see BareMetalTaskScheduler); only bm-thread may have any.
        const bool allowed = r.c.misses != 0 && m == kBmThread;
        std::printf("%-10s maxErr %.3e  bg %u fg %u stolen %u misses %u restarts %u maxQueued %u  %s\n",
                    names[m], r.maxErr, r.c.backgroundSteps, r.c.foregroundSteps, r.c.stolenSteps, r.c.misses,
                    r.c.restarts, r.c.maxQueued, exact ? "OK" : (allowed ? "(errors expected: misses)" : "FAIL"));
        if (!exact && !allowed) ++failures;
    }
    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
