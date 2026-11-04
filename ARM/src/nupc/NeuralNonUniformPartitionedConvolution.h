//
// Created by sedur on 12/10/2026.
//

#ifndef MONOLITH_NEURALNONUNIFORMPARTITIONEDCONVOLUTION_H
#define MONOLITH_NEURALNONUNIFORMPARTITIONEDCONVOLUTION_H
#include "Convolution.h"
#include <cmath>
#include <cstring>
#include <cstdint>
// NUPC_BARE_METAL (SAM / ADSP-SC589 ARM core, see NupcPlatform.h): no OS threads, mutexes or <chrono>. The
// BareMetalTaskScheduler replaces ThreadedTaskScheduler and NupcHost (control-thread/audio-thread double
// buffering) is not built.
#ifdef NUPC_BARE_METAL
#include "NupcPlatform.h"
#include "BareMetalTaskScheduler.hpp"
#else
#include "ThreadedTaskScheduler.hpp"
#include <chrono>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#endif
#include "york_minster_ir.h"
#ifndef BUFFER_SIZE_SAMPLES
#define BUFFER_SIZE_SAMPLES 32
#endif

namespace neural {
namespace arm {
namespace detail {

// Converts up to `maxSamples` of a bit-pattern IR table (see york_minster_ir.h) into real `float`s written to
// `dest`, returning how many samples were actually converted (min(available, maxSamples)). This is the "load a
// specific sub-amount" step: the source table may be far larger (and live in external memory, see the
// YORK_MINSTER_IR_SECTION comment in that header) than what any one caller currently wants staged into ordinary
// working `float` memory, so this only ever reads/converts the requested prefix, never the whole table.
//
// Each sample is converted with a memcpy (not a bulk reinterpret_cast over the whole array) so this is
// well-defined under strict aliasing on every compiler/target -- reinterpreting a `uint32_t*` as a `float*` and
// dereferencing it is technically undefined behaviour in C++ even though it happens to work on most ARM/x86 GCC
// builds; memcpy of the same size is the portable, standard-sanctioned way to reinterpret bits as a float.
inline std::size_t loadIrSamplePrefix(const uint32_t* bits, std::size_t available, float* dest,
                                       std::size_t maxSamples)
{
    const std::size_t n = maxSamples < available ? maxSamples : available;
    for (std::size_t i = 0; i < n; ++i)
    {
        const uint32_t bitPattern = bits[i];
        std::memcpy(&dest[i], &bitPattern, sizeof(float));
    }
    return n;
}

// A TaskScheduler with no worker threads of its own: every method it implements runs on whichever call stack
// calls it, and every one of those call stacks is the audio-processing call (NupcConvolver::process() ->
// NonUniformPartitionedConvolver::process() -> PartitionedConvolutionCore::processChunk()/advance()/join()).
// Nothing here ever runs from an interrupt or a second thread, so there is no concurrent access to a task's
// state to guard against -- correctness does not depend on timing, only on advance() being called often enough
// (once per audio buffer) to finish a task's steps before join() needs its result.
//
// Steps are spread "over the callbacks" (see NupcConvolver's class comment / ADR-0027), but *how many* steps run
// per callback is governed by a cycle budget (setCycleBudget()), not a fixed count: a single "step" can be a
// forward/inverse FFT radix stage or a MAC chunk (see PartitionedConvolutionCore::Stage), and those vary a lot in
// cost -- an 8192-sample stage's FFT stages cost far more than a 512-sample stage's MAC chunk. Doing exactly "one
// step per task per callback" (the previous design) therefore made each callback's cost swing with however many
// stages happened to be pending and how expensive *their* next step was, which is both unpredictable and, worse,
// can exceed the audio callback's own time budget on the expensive end.
//
// advance() instead keeps running steps, round-robin across whichever tasks still have steps left, until either
// (a) there is nothing left to do, or (b) the time spent this call reaches the cycle budget -- so it spends *up
// to* the budget doing useful background work (pulling future audio buffers' FFTs forward when there's headroom)
// but never knowingly blows past it other than by the cost of whichever single step was in flight when the
// budget was reached (steps are individually bounded -- see kMaxMacSteps and the FFT's own radix-stage count in
// Convolution.h/fft.hpp -- so that overshoot is itself bounded to roughly one radix stage or one MAC chunk, not a
// whole stage's work). join() remains the unconditional correctness backstop: whatever advance() hasn't finished
// by the time a stage's output is needed, join() finishes inline, right there, exactly as if there were no
// scheduler at all -- so an incorrectly-tiny (or zero) budget can only ever cost performance, never correctness.
//
// The round-robin start position (cursor_) is carried across calls so that if the budget is tight enough to stop
// partway through the task list, the *next* call resumes with the tasks that lost out last time, rather than
// always favouring the same early tasks.
class AudioThreadTaskScheduler : public neural::arm::TaskScheduler
{
public:
    enum { kMaxTasks = 16 }; // generous: only stages >= kMinTaskBlockSize become tasks, and there are few of those

    AudioThreadTaskScheduler() : count_(0), cursor_(0), budgetNanos_(~std::uint64_t(0)) {}

    // Nanoseconds advance() may spend per call before it stops (see class comment). Default is effectively
    // unlimited, so a scheduler that's never had this called on it behaves like the simple "run everything
    // pending" scheduler -- callers that care about a real-time deadline must call this once, after
    // construction, with a real budget (see NupcConvolver::prepare() for how that budget is derived).
    void setBudgetNanos(std::uint64_t nanos) { budgetNanos_ = nanos; }

    // Forgets every task, so the owning convolver can be re-prepared. Without this create() would eventually run out
    // of slots (kMaxTasks) after a few re-prepares. Only legal while nothing is calling advance()/join().
    void clear() { count_ = 0; cursor_ = 0; }

    // Monotonic nanosecond clock, used by AudioThreadTaskScheduler to bound how much of each audio buffer it
    // spends on background FFT work (see the "time budget" comment on that class).
    //
    // steady_clock is guaranteed never to jump backwards or be adjusted (unlike system_clock, which NTP can step),
    // which is exactly what a duration measurement needs. On Linux/ARM it resolves to clock_gettime(CLOCK_MONOTONIC)
    // via the vDSO -- a userspace read of a kernel-maintained page, no syscall, roughly 20-30ns -- so it is cheap
    // enough to call once per step, though not inside an inner loop.
    //
    // Returning 64-bit nanoseconds means callers need no wraparound care: 2^64 ns is ~584 years.
    static std::uint64_t readNanos()
    {
#ifdef NUPC_BARE_METAL
        return nupc_read_nanos(); // PMU cycle counter scaled by the core clock
#else
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
    }

    unsigned create(StepFn fn, void* context, unsigned steps) override;

    void start(unsigned task, unsigned /*deadlineFrames*/) override;

    void join(unsigned task) override;

    // Called once per audio buffer from PartitionedConvolutionCore::processChunk(). Runs steps round-robin across
    // pending tasks until either none are left or the cycle budget is spent (see class comment).
    void advance() override;

private:
    struct Task
    {
        StepFn fn;
        void* context;
        unsigned totalSteps;
        unsigned nextStep;
    };
    Task tasks_[kMaxTasks];
    unsigned count_;
    unsigned cursor_;
    std::uint64_t budgetNanos_;
};

} // namespace detail
} // namespace arm
} // namespace neural

// Convolves a stereo signal with an impulse response file using non-uniform partitioned convolution (Gardner): small
// partitions at the start of the response for zero latency, growing to large ones later for efficiency, so long
// reverbs cost far less than with Convolver. The large stages (512 samples and up) run as background tasks: they
// start when their input block is complete and are joined when their output is first needed, so the host can spread
// their FFTs over worker threads or over the callbacks instead of doing them all in one buffer every 8192 samples
// (#192, ADR-0027). The output is the same whoever runs them. A mono IR feeds both channels; a stereo IR convolves left
// with left and right with right. An IR at another sample rate is resampled when the block is prepared, and IRs
// longer than 10 s are cut short. With no file the block passes its input through.
class NupcConvolver
{
public:
    enum
    {
        kMinBlockSize = 32,  // the first stage, run directly in the time domain: no latency
        kMaxBlockSize = 65536, // the largest partitions: next power of two above one second (48000) at 48 kHz.
                               // One such FFT is 131072 points, so it only works because the stage runs as a
                               // background task on a worker (see ThreadedTaskScheduler), never inline in the callback.
        kBlocksPerStage = 4,
        // Stages of at least this many samples run as background tasks spread over callbacks by
        // AudioThreadTaskScheduler::advance() (see its class comment) instead of running inline the instant
        // their block fills. layout()'s very first stage (kMinBlockSize taps, direct time domain, see
        // NonUniformPartitionedConvolver::configure()) is never an FFT stage and never a task regardless of this
        // value -- it is the zero-latency "head" and always runs its per-sample multiply-adds inline, which is
        // fine: that cost is small and perfectly uniform (BUFFER_SIZE_SAMPLES * headTaps MACs every buffer, no
        // spikes). Every stage *after* the head is an FFT stage, and the smallest one layout() can ever produce
        // is 2 * kMinBlockSize (the ramp doubles from kMinBlockSize upward) -- so setting this to 2 * kMinBlockSize
        // means literally every FFT stage, however small, becomes a background task whose forward-FFT/MAC/inverse-
        // FFT steps get spread across multiple audio buffers via the cycle budget, rather than bursting an entire
        // stage's FFT work into the single callback where its block happens to complete. That burst-on-completion
        // behaviour (previously only avoided above 512 samples) is exactly what caused the reported spiky 27%-to-
        // 100%+ callback cost: short responses produce only small ramp stages (e.g. 64, 128 samples) that used to
        // run entirely inline. There is no real downside to scheduling even tiny stages as tasks here: unlike a
        // thread-pool scheduler (which this isn't), AudioThreadTaskScheduler has no synchronisation/wakeup cost
        // per task, so "task" vs "inline" is purely a question of *when* the steps run, not how expensively.
        kMinTaskBlockSize = 2 * kMinBlockSize,
        kMaxSeconds = 10
    };

    // Fraction of one audio buffer's time (BUFFER_SIZE_SAMPLES / sampleRate) that AudioThreadTaskScheduler may
    // spend per callback pulling background FFT steps forward (see its class comment in detail:: above). The
    // remainder is headroom for process()'s own fixed-cost work (the head-tap convolution, ring buffer, mixing)
    // plus whatever margin the rest of the audio ISR and other modules need -- 0.70 was chosen as a starting
    // point that leaves generous headroom; tune it (and re-measure on hardware) if background FFT work is still
    // not keeping up, or lower it further if other work in the same callback needs more room.
    static constexpr float kBackgroundBudgetFraction = 0.70f;

    NupcConvolver() : active_(false), mix_(1.0f), gainDb_(0.0f), normalise_(true), irScale_(1.0f) {}

    ~NupcConvolver() { releaseMemory(); }

    // The convolver's tasks hold raw pointers into `memory_` and into tasks_, so neither may be copied or moved.
    NupcConvolver(const NupcConvolver&) = delete;
    NupcConvolver& operator=(const NupcConvolver&) = delete;

    enum Scheduler
    {
        kSchedulerAuto,        // threaded workers if the machine has a spare core, else the audio thread
        kSchedulerThreaded,    // worker threads (falls back to the audio thread if there is no spare core)
        kSchedulerAudioThread  // background FFT steps spread over the audio callbacks within a time budget
    };

    // Everything prepare() can be told about the convolver. The impulse response itself is always the embedded
    // York Minster one (york_minster_ir.h); `irLength` says how much of it to use. The defaults reproduce the
    // engine's normal configuration.
    struct Config
    {
        std::size_t irLength;       // samples of the embedded IR to load, clamped to what it holds; 0 = all of it
        std::size_t minBlock;       // first (direct, zero-latency) stage's block size; power of two
        std::size_t maxBlock;       // largest FFT partition; power of two >= minBlock
        std::size_t blocksPerStage; // partitions per ramp stage, >= 2
        std::size_t minTaskBlock;   // stages of at least this many samples run as background tasks
        Scheduler   scheduler;
        unsigned    workers;        // worker threads for the threaded scheduler; 0 = hardware_concurrency() - 1
        std::size_t foregroundFrames; // bare metal only: stages with a deadline below this many frames run on the callback

        Config()
            : irLength(0), minBlock(kMinBlockSize), maxBlock(kMaxBlockSize), blocksPerStage(kBlocksPerStage),
              minTaskBlock(kMinTaskBlockSize), scheduler(kSchedulerAuto), workers(0), foregroundFrames(512)
        {}
    };

    // What is currently loaded, plus the scheduler's counters. Plain data so a control thread can report it.
    struct Stats
    {
        bool active = false;
        std::size_t irLength = 0, minBlock = 0, maxBlock = 0, blocksPerStage = 0, minTaskBlock = 0;
        std::size_t numStages = 0, paddedLength = 0, workingFloats = 0;
        bool threaded = false;
        unsigned workers = 0;
        float irScale = 1.0f;
        std::uint64_t stolenBack = 0, blockedOnWorker = 0, workerBusyNanos = 0, workerTasks = 0;
    };

    // Prepare with the default Config.
    bool prepare(float sampleRate);

    // (Re)build the convolver. Allocates and starts threads, so never call this from an audio thread, and never
    // while another thread is inside process() on this object -- NupcHost below does the double-buffering that
    // makes a live reconfiguration safe.
    bool prepare(float sampleRate, const Config& config);

    // Stops workers and frees the working memory; process() passes audio through until the next prepare().
    void release() { releaseMemory(); }

    Stats stats() const;
#ifdef NUPC_BARE_METAL
    void resetCounters() { bmTasks_.resetCounters(); }

    // Background tier (see BareMetalTaskScheduler): call from a low-priority task or the idle loop, never from the
    // audio callback. Runs up to maxSteps queued steps and returns how many ran.
    unsigned serviceBackground(unsigned maxSteps = 1) { return active_ ? bmTasks_.serviceBackground(maxSteps) : 0; }
    bool backgroundIdle() const { return bmTasks_.backgroundIdle(); }
    neural::arm::detail::BareMetalTaskScheduler::Counters schedulerCounters() const { return bmTasks_.counters(); }
    bool active() const { return active_; }
#else
    void resetCounters() { threadedTasks_.resetCounters(); }
#endif

    void reset();

    // Convolves one buffer of BUFFER_SIZE_SAMPLES frames from `in` into `out`, mixing dry and wet per mix_.
    // `in` and `out` may be the same pointer (in-place). When inactive, `in` is copied through unchanged, so a
    // caller can wire this into a signal path unconditionally and get passthrough until prepare() succeeds.
    //
    // Deliberately channel-agnostic: the caller picks which channel this runs on (see ARMAudioEngine's
    // processIMX8MP()), so this class needs no knowledge of the engine's channel enums.
    void process(const float* in, float* out);

    // In-place convenience overload.
    void process(float* buffer) { process(buffer, buffer); }

//    void setParam(uint32_t index, float value);

private:
    float linearGain() const { return static_cast<float>(std::pow(10.0, gainDb_ / 20.0)); }

    // Releases the convolver's working memory. The convolver must be told to forget it first: its stages hold
    // raw pointers carved out of this block, so it must never be used again between the free and a re-prepare().
    void releaseMemory()
    {
        // Before anything else: the workers run steps that dereference raw Stage pointers into `memory_`, so
        // they must all be stopped and joined before that memory goes away or is rebuilt by a re-prepare().
#ifdef NUPC_BARE_METAL
        // Deactivate first so a preempting audio callback passes audio through, then drop every task. The caller
        // must not be inside serviceBackground() on another context (see BareMetalTaskScheduler::clear()).
        active_ = false;
        bmTasks_.clear();
        tasks_.clear();
        convolver_.invalidate();
        nupc_arena_reset(); // working block and Ne10 configs all come from the arena
        memory_ = 0;
        usingThreads_ = false;
#else
        threadedTasks_.stopWorkers();
        threadedTasks_.clearTasks();
        tasks_.clear();
        convolver_.invalidate();
        active_ = false;
        delete[] memory_;
        memory_ = 0;
        usingThreads_ = false;
#endif
    }

    // Scales the louder channel's response to unit energy, so different IRs come out at similar levels.
    static float unitEnergyScale(const float* ir, std::size_t length)
    {
        double energy = 0.0;
		for (std::size_t i = 0; i < length; ++i)
			energy += static_cast<double>(ir[i]) * ir[i];
        return energy > 1e-12 ? static_cast<float>(1.0 / std::sqrt(energy)) : 1.0f;
    }

    // Two schedulers, one used per prepare() (see the choice made there): the threaded one runs background FFT
    // work on worker threads and is the normal case, the audio-thread one is the fallback for a machine with no
    // spare core to put a worker on.
#ifdef NUPC_BARE_METAL
    neural::arm::detail::BareMetalTaskScheduler bmTasks_; // foreground steps in the callback, background via serviceBackground()
#else
    neural::arm::detail::ThreadedTaskScheduler threadedTasks_;
#endif
    neural::arm::detail::AudioThreadTaskScheduler tasks_; // runs background FFT steps from the audio callback
    neural::arm::NonUniformPartitionedConvolver convolver_;
    float* memory_ = 0; // the convolver's working block, carved up by prepare(); owned here
    Config config_;
    std::size_t loadedIrLength_ = 0;
    std::size_t memoryFloats_ = 0;
    bool usingThreads_ = false;
    bool active_;
    float mix_;
    float gainDb_;
    bool normalise_;
    float irScale_;
//    seds::SmoothedValue mixSmoothed_;
//    seds::SmoothedValue gainSmoothed_;
};

#ifndef NUPC_BARE_METAL
// Runs a NupcConvolver that can be reconfigured while audio is flowing.
//
// NupcConvolver::prepare() allocates memory, builds FFT plans and starts threads, so it cannot run on the audio
// thread, and the audio thread cannot be inside process() on an engine that is being rebuilt. NupcHost therefore owns
// two engines and ping-pongs between them:
//
//   configure()  (control thread)  builds the new configuration on the idle engine, publishes it with one atomic
//                                  pointer exchange, waits until the audio thread has left the old engine, then
//                                  frees the old one.
//   process()    (audio thread)    reads the published pointer and runs it. It never allocates, locks or waits;
//                                  the cost of being swappable is three atomic operations per buffer.
//
// The new engine starts with empty history, so a swap is audible as the old reverb tail being cut off. Only one
// large engine is kept alive in steady state; both exist for the duration of a configure().
//
// process() must only be called from one thread. configure()/stats() may be called from any thread (serialised).
class NupcHost
{
public:
    NupcHost() : current_(nullptr), busy_(false), epoch_(0), activeIdx_(-1) {}

    // Configure with the default Config. Mirrors NupcConvolver::prepare(float) so existing callers are unchanged.
    bool prepare(float sampleRate) { return configure(sampleRate, NupcConvolver::Config()); }

    // Returns false (and leaves the running configuration untouched) if the new one could not be built, or if
    // the audio thread did not leave the old engine within the timeout -- in which case the old engine is kept
    // allocated rather than risk freeing memory the audio thread may still be using.
    bool configure(float sampleRate, const NupcConvolver::Config& config)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int idx = activeIdx_ == 0 ? 1 : 0;
        if (!slots_[idx])
            slots_[idx].reset(new NupcConvolver());
        if (!slots_[idx]->prepare(sampleRate, config))
        {
            slots_[idx]->release();
            return false;
        }

        NupcConvolver* const old = current_.exchange(slots_[idx].get(), std::memory_order_seq_cst);
        const int oldIdx = activeIdx_;
        activeIdx_ = idx;
        if (!old)
            return true;

        // Wait for any process() call that may still hold `old`. busy_ is set before the pointer is read and
        // cleared after the call, all seq_cst: a call that read `old` is therefore visible as busy_ here until it
        // finishes, and one that starts after the exchange reads the new pointer. epoch_ lets us stop waiting once
        // that particular call has ended even if another one has already begun.
        const std::uint32_t e0 = epoch_.load(std::memory_order_seq_cst);
        for (int waited = 0; busy_.load(std::memory_order_seq_cst) && epoch_.load(std::memory_order_seq_cst) == e0; ++waited)
        {
            if (waited > 50000) // 5 s at 100 us: the audio thread is stuck; do not free what it may be using
                return true;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        slots_[oldIdx]->release();
        return true;
    }

    // Audio thread.
    void process(const float* in, float* out)
    {
        busy_.store(true, std::memory_order_seq_cst);
        NupcConvolver* const c = current_.load(std::memory_order_seq_cst);
        if (c)
            c->process(in, out);
        else if (out != in)
            std::memcpy(out, in, BUFFER_SIZE_SAMPLES * sizeof(float));
        epoch_.fetch_add(1, std::memory_order_seq_cst);
        busy_.store(false, std::memory_order_seq_cst);
    }
    void process(float* buffer) { process(buffer, buffer); }

    NupcConvolver::Stats stats() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return activeIdx_ >= 0 ? slots_[activeIdx_]->stats() : NupcConvolver::Stats();
    }

    void resetCounters()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (activeIdx_ >= 0)
            slots_[activeIdx_]->resetCounters();
    }

    // Clears the running engine's history. Only legal while no thread is inside process() (the audio thread must be
    // stopped): unlike configure() it does not wait for the audio thread.
    void reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (activeIdx_ >= 0)
            slots_[activeIdx_]->reset();
    }

private:
    mutable std::mutex mutex_;
    std::unique_ptr<NupcConvolver> slots_[2];
    std::atomic<NupcConvolver*> current_;
    std::atomic<bool> busy_;
    std::atomic<std::uint32_t> epoch_;
    int activeIdx_;
};
#endif // !NUPC_BARE_METAL

#endif //MONOLITH_NEURALNONUNIFORMPARTITIONEDCONVOLUTION_H
