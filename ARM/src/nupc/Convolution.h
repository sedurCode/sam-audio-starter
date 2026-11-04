/*
 * Convolution.h
 *
 *  Created on: 5 Oct 2026
 *      Author: sedur
 */

#ifndef NUPC_ARM_CONVOLUTION_H_
#define NUPC_ARM_CONVOLUTION_H_
#include "fft.hpp"

namespace neural {
namespace arm {

// Where a convolver's background tasks go (see "Background tasks" above). The block that owns the convolver
// implements it over its host's worker; dsp/ knows nothing of the SDK. create()/start()/join() are all called
// from the thread that processes audio, create() only while preparing.
//
// A host with no real worker threads (e.g. a bare-metal audio ISR with no RTOS) can still spread a task's steps
// out over time by implementing advance(): PartitionedConvolutionCore::processChunk() calls it once per audio
// chunk, after start() and before the task is due, so an implementation can run one (or a few) of the task's
// steps per audio callback instead of all of them at once. join() remains the correctness backstop regardless:
// whatever advance() hasn't finished by the time the output is needed, join() finishes there and then, so a
// scheduler that only ever runs steps from join() (never overriding advance()) is still correct -- advance() is
// purely a way to move work earlier, never a requirement.
class TaskScheduler
{
public:
    typedef void (*StepFn)(void* context, unsigned step);

    // A task of `steps` steps; returns its handle (not 0), or 0 if there is no room.
    virtual unsigned create(StepFn fn, void* context, unsigned steps) = 0;
    // Queue the task to finish within `deadlineFrames` frames.
    virtual void start(unsigned task, unsigned deadlineFrames) = 0;
    // Finish the task: run its remaining steps here or wait for the thread that is running them.
    virtual void join(unsigned task) = 0;
    // Called once per audio chunk (see processChunk()): an implementation with no worker threads of its own can
    // use this to run a bounded slice of whichever task(s) are pending, spreading their FFT steps across
    // callbacks instead of doing them all in join(). The default does nothing (a thread-backed scheduler has no
    // need for it: its worker(s) make progress independently of audio callbacks).
    virtual void advance() {}

protected:
    ~TaskScheduler() {}
};

namespace detail {

inline std::size_t ceilDiv(std::size_t a, std::size_t b) { return b == 0 ? 0 : (a + b - 1) / b; }

// The shared engine: a direct head of `headTaps` taps, then FFT stages, all output delayed by `latency` samples.
class PartitionedConvolutionCore
{
public:
    enum { kMaxStages = 40 };

    struct Config
    {
        std::size_t headTaps;
        std::size_t numStages;
        std::size_t blockSizes[kMaxStages];
        std::size_t blockCounts[kMaxStages];
        std::size_t offsets[kMaxStages];
        std::size_t latency;

        Config() : headTaps(0), numStages(0), latency(0)
        {
            for (int i = 0; i < kMaxStages; ++i)
            {
                blockSizes[i] = 0;
                blockCounts[i] = 0;
                offsets[i] = 0;
            }
        }
    };

    PartitionedConvolutionCore()
        : numStages_(0), headTaps_(0), latency_(0), maxFrames_(0), taps_(0), input_(0), ring_(0), ringSize_(0),
          ringPos_(0), scratch_(0), now_(0), scheduler_(0), minTaskBlock_(0), ready_(false)
    {
        for (int i = 0; i < kMaxStages; ++i)
            cfgs_[i] = 0;
    }

    ~PartitionedConvolutionCore() { releaseCfgs(); }

    // The tasks point at this object's stages and the cfgs are owned, not shared, so copying is forbidden
    // (this was already the documented contract on setScheduler(); now it is enforced).
    PartitionedConvolutionCore(const PartitionedConvolutionCore&) = delete;
    PartitionedConvolutionCore& operator=(const PartitionedConvolutionCore&) = delete;

    // Frees the Ne10 configurations and marks the convolver unprepared, so process() outputs silence.
    // Replaces assigning a default-constructed core over this one, which copy-deletion now forbids.
    void invalidate()
    {
        releaseCfgs();
        ready_ = false;
        numStages_ = 0;
    }

    // Before prepare(): stages of at least `minTaskBlock` samples run as tasks of `scheduler` (null: all inline).
    // The convolver must not be moved or copied once prepared (the tasks point at it).
    void setScheduler(TaskScheduler* scheduler, std::size_t minTaskBlock)
    {
        scheduler_ = scheduler;
        minTaskBlock_ = minTaskBlock;
    }

    // Stages must be power-of-two blocks, each starting at least one block (less the latency) into the response.
    static bool valid(const Config& config)
    {
        if (config.numStages > static_cast<std::size_t>(kMaxStages))
            return false;
        for (std::size_t i = 0; i < config.numStages; ++i)
        {
            if (!RealFft::isPowerOfTwo(config.blockSizes[i]) || config.blockCounts[i] == 0)
                return false;
            if (config.offsets[i] + config.latency < config.blockSizes[i])
                return false;
        }
        return config.latency == 0 || config.headTaps == 0;
    }

    static std::size_t requiredFloats(const Config& config, std::size_t maxFrames)
    {
        if (maxFrames == 0)
            maxFrames = 1;
        std::size_t total = config.headTaps + historyLength(config.headTaps) + maxFrames + 3; // +3: alignCursor padding
        std::size_t largest = 0;
        for (std::size_t i = 0; i < config.numStages; ++i)
        {
            const std::size_t b = config.blockSizes[i];
            const std::size_t spectrum = RealFft::spectrumFloats(2 * b);
            // filters + history (K spectra each), frame (2B), acc (spectrum), work (2B -- see Stage::work).
            // tableFloats() is 0 under Ne10 (it owns its twiddles inside the cfg) but is kept in the sum so
            // this stays correct if the FFT backend is ever changed again.
            total += RealFft::tableFloats(2 * b) + 2 * config.blockCounts[i] * spectrum + 4 * b + spectrum; // + acc
            if (b > largest)
                largest = b;
        }
        if (config.numStages > 0)
            total += RealFft::spectrumFloats(2 * largest) + ringLength(config);
        return total;
    }

    // Ne10's ARMv7 NEON kernels load spectra with 8-byte alignment qualifiers (e.g. "[p_fin0:64]" in
    // NE10_fft_float32.neon.s), which the hardware enforces: a misaligned buffer faults rather than
    // merely running slowly. The block base from new[] is 16-byte aligned and the spectrum stride
    // (n + 2 floats, n even) preserves 8-byte alignment, but the preceding scalar regions do not --
    // historyLength() is headTaps - 1, an odd float count -- so the cursor is realigned here before
    // anything Ne10 will touch. 4 floats (16 bytes) rather than 2 so the loads are optimally aligned
    // too, not merely legal.
    static float* alignCursor(float* p, float* base)
    {
        const std::size_t used = static_cast<std::size_t>(p - base);
        const std::size_t pad = (4 - (used & 3)) & 3;
        return p + pad;
    }

    // Carves `memory` up, copies the response's partitions in and transforms them. Partitions beyond `irLength`
    // are zero-padded.
    bool prepare(const Config& config, const float* ir, std::size_t irLength, float* memory, std::size_t memoryFloats,
                 std::size_t maxFrames)
    {
        ready_ = false;
        releaseCfgs(); // a re-prepare (IR swap) must not leak the previous stages' Ne10 configurations
        if (!valid(config) || memory == 0 || (irLength > 0 && ir == 0))
            return false;
        if (maxFrames == 0)
            maxFrames = 1;
        if (memoryFloats < requiredFloats(config, maxFrames))
            return false;

        numStages_ = config.numStages;
        headTaps_ = config.headTaps;
        latency_ = config.latency;
        maxFrames_ = maxFrames;

        float* cursor = memory;
        taps_ = cursor;
        cursor += headTaps_;
        input_ = cursor;
        cursor += historyLength(headTaps_) + maxFrames_;
        cursor = alignCursor(cursor, memory);

        // The head's taps, reversed, so each output sample is a forward dot product over the input history.
        for (std::size_t i = 0; i < headTaps_; ++i)
        {
            const std::size_t tap = headTaps_ - 1 - i;
            taps_[i] = tap < irLength ? ir[tap] : 0.0f;
        }

        std::size_t largest = 0;
        for (std::size_t s = 0; s < numStages_; ++s)
            if (config.blockSizes[s] > largest)
                largest = config.blockSizes[s];
        scratch_ = 0;
        ring_ = 0;
        ringSize_ = 0;
        if (numStages_ > 0)
        {
            scratch_ = cursor;
            cursor += RealFft::spectrumFloats(2 * largest);
            ringSize_ = ringLength(config);
            ring_ = cursor;
            cursor += ringSize_;
        }

        for (std::size_t s = 0; s < numStages_; ++s)
        {
            Stage& stage = stages_[s];
            stage.blockSize = config.blockSizes[s];
            stage.count = config.blockCounts[s];
            stage.offset = config.offsets[s];
            const std::size_t n = 2 * stage.blockSize;
            const std::size_t spectrum = RealFft::spectrumFloats(n);
            cfgs_[s] = ne10_fft_alloc_r2c_float32(static_cast<ne10_int32_t>(n));
            if (cfgs_[s] == 0) // Ne10 allocates the cfg itself; nothing else has been published yet
            {
                releaseCfgs();
                return false;
            }
            stage.fft.attach(n, cfgs_[s]);
            cursor += RealFft::tableFloats(n);
            stage.filters = cursor;
            cursor += stage.count * spectrum;
            stage.history = cursor;
            cursor += stage.count * spectrum;
            stage.frame = cursor;
            cursor += n;
            stage.acc = cursor;
            cursor += spectrum;
            stage.work = cursor;
            cursor += n;
            stage.owner = this;
            stage.index = s;
            stage.fill = 0;
            stage.pending = false;
            // Ne10's transforms run to completion and cannot be paused, so each is a single step. The MAC
            // phase is still divisible (macSteps), which is what the scheduler spreads when it needs to.
            stage.forwardSteps = 1;
            stage.inverseSteps = 1;
            stage.partitionsPerStep = ceilDiv(stage.count, stage.count < static_cast<std::size_t>(kMaxMacSteps) ? stage.count : static_cast<std::size_t>(kMaxMacSteps));
            stage.macSteps = ceilDiv(stage.count, stage.partitionsPerStep);
            stage.task = 0;
            if (scheduler_ != 0 && stage.blockSize >= minTaskBlock_)
            {
                const std::size_t steps = stage.forwardSteps + stage.macSteps + stage.inverseSteps;
                stage.task = scheduler_->create(&PartitionedConvolutionCore::stepTrampoline, &stage, static_cast<unsigned>(steps));
            }

            // Each partition, zero-padded to 2B, transformed.
            //
            // No 1/N scaling here, deliberately. The old hand-written FFT left the inverse unnormalised, so the
            // 1/N had to be folded into the filter spectra. Ne10 is the other convention: its c2r applies
            // one_by_N internally (see ne10_radix4_c2r_c / ne10_radix8_c2r_c in NE10_rfft_float32.c), so folding
            // it in here as well would normalise twice and attenuate every stage by its own 1/N -- which is
            // exactly what the offline test caught (a 512-sample stage came out 1024x too quiet).
            for (std::size_t k = 0; k < stage.count; ++k)
            {
                const std::size_t start = stage.offset + k * stage.blockSize;
                for (std::size_t i = 0; i < n; ++i)
                {
                    const std::size_t tap = start + i;
                    scratch_[i] = (i < stage.blockSize && tap < irLength) ? ir[tap] : 0.0f;
                }
                stage.fft.forward(scratch_, stage.filters + k * spectrum);
            }
        }
        for (std::size_t s = numStages_; s < static_cast<std::size_t>(kMaxStages); ++s)
            stages_[s] = Stage();

        ready_ = true;
        reset();
        return true;
    }

    bool ready() const { return ready_; }
    std::size_t latency() const { return latency_; }

    void reset()
    {
        if (!ready_)
            return;
        for (std::size_t s = 0; s < numStages_; ++s)
            if (stages_[s].pending)
            {
                scheduler_->join(stages_[s].task); // its output is discarded
                stages_[s].pending = false;
            }
        now_ = 0;
        const std::size_t inputFloats = historyLength(headTaps_) + maxFrames_;
        for (std::size_t i = 0; i < inputFloats; ++i)
            input_[i] = 0.0f;
        for (std::size_t i = 0; i < ringSize_; ++i)
            ring_[i] = 0.0f;
        ringPos_ = 0;
        for (std::size_t s = 0; s < numStages_; ++s)
        {
            Stage& stage = stages_[s];
            const std::size_t historyFloats = stage.count * RealFft::spectrumFloats(2 * stage.blockSize);
            for (std::size_t i = 0; i < historyFloats; ++i)
                stage.history[i] = 0.0f;
            for (std::size_t i = 0; i < 2 * stage.blockSize; ++i)
                stage.frame[i] = 0.0f;
            for (std::size_t i = 0; i < 2 * stage.blockSize; ++i)
                stage.work[i] = 0.0f;
            stage.fill = 0;
            stage.newest = 0;
        }
    }

    // `in` and `out` may be the same buffer.
    void process(const float* in, float* out, std::size_t numFrames)
    {
        if (!ready_)
        {
            for (std::size_t i = 0; i < numFrames; ++i)
                out[i] = 0.0f;
            return;
        }
        while (numFrames > 0)
        {
            const std::size_t chunk = numFrames < maxFrames_ ? numFrames : maxFrames_;
            processChunk(in, out, chunk);
            in += chunk;
            out += chunk;
            numFrames -= chunk;
        }
    }

private:
    struct Stage
    {
        Stage()
            : blockSize(0), count(0), offset(0), filters(0), history(0), acc(0), frame(0), work(0), fill(0), newest(0), forwardSteps(0),
              macSteps(0), partitionsPerStep(1), inverseSteps(0), task(0), pending(false), due(0), owner(0), index(0)
        {
        }

        std::size_t blockSize; // B
        std::size_t count;     // K partitions
        std::size_t offset;    // D: where the stage's first partition starts in the response
        RealFft fft;           // 2B points
        float* filters;        // K partition spectra
        float* history;        // the K most recent input spectra, a ring; `newest` is the latest
        float* acc;            // the sum over partitions (the inverse transform goes to `work`, not here)
        float* frame;          // the previous block then the one filling: 2B samples
        float* work;           // 2B scratch: the frame snapshot in, then the inverse transform's output
        std::size_t fill;      // samples in the filling block
        std::size_t newest;
        // The stage's work as steps: forwardSteps, then macSteps groups of partitionsPerStep partitions, then
        // inverseSteps.
        std::size_t forwardSteps;
        std::size_t macSteps;
        std::size_t partitionsPerStep;
        std::size_t inverseSteps;
        // Background task (0: the stage runs all its steps when its block fills).
        unsigned task;
        bool pending;           // a started task whose output is not placed yet
        std::size_t due;        // `now_` when its first output sample is needed
        PartitionedConvolutionCore* owner;
        std::size_t index;
    };

    static std::size_t historyLength(std::size_t headTaps) { return headTaps > 0 ? headTaps - 1 : 0; }

    // Pending stage output covers at most D + B + latency samples past the current chunk.
    static std::size_t ringLength(const Config& config)
    {
        std::size_t longest = 0;
        for (std::size_t i = 0; i < config.numStages; ++i)
        {
            const std::size_t reach = config.offsets[i] + config.blockSizes[i] + config.latency;
            if (reach > longest)
                longest = reach;
        }
        return longest;
    }

    void processChunk(const float* in, float* out, std::size_t numFrames)
    {
        // Give a non-thread-backed scheduler a chance to run a slice of any pending task's steps now, before this
        // chunk's own work below (which may join() a task that is due) -- see TaskScheduler::advance().
        if (scheduler_)
            scheduler_->advance();

        const std::size_t history = historyLength(headTaps_);
        float* current = input_ + history;
        for (std::size_t i = 0; i < numFrames; ++i)
            current[i] = in[i];

        // 1. The head, directly.
        for (std::size_t n = 0; n < numFrames; ++n)
        {
            const float* x = input_ + n;
            float sum = 0.0f;
            for (std::size_t t = 0; t < headTaps_; ++t)
                sum += taps_[t] * x[t];
            out[n] = sum;
        }

        // 2. What earlier blocks' stages scheduled for these samples.
        if (ringSize_ > 0)
        {
            std::size_t pos = ringPos_;
            for (std::size_t n = 0; n < numFrames; ++n)
            {
                out[n] += ring_[pos];
                ring_[pos] = 0.0f;
                if (++pos == ringSize_)
                    pos = 0;
            }
        }

        // 2b. Background stages whose output is needed in this chunk are joined and placed.
        for (std::size_t s = 0; s < numStages_; ++s)
            if (stages_[s].pending && stages_[s].due - now_ < numFrames)
                finishStage(stages_[s], out, numFrames);

        // 3. The stages: every block that fills in this chunk is convolved and its output placed.
        for (std::size_t s = 0; s < numStages_; ++s)
        {
            Stage& stage = stages_[s];
            const std::size_t b = stage.blockSize;
            std::size_t consumed = 0;
            while (consumed < numFrames)
            {
                std::size_t take = b - stage.fill;
                if (take > numFrames - consumed)
                    take = numFrames - consumed;
                float* dest = stage.frame + b + stage.fill;
                for (std::size_t i = 0; i < take; ++i)
                    dest[i] = current[consumed + i];
                stage.fill += take;
                consumed += take;
                if (stage.fill == b)
                {
                    if (stage.pending) // the previous block's output goes in the ring until it is needed
                        finishStage(stage, out, numFrames);
                    startStage(stage, now_ + consumed);
                    // The block's first sample is at consumed - B (negative if it began in an earlier chunk); its
                    // output belongs D + latency samples later, which is never before `consumed`.
                    if (stage.task == 0 || stage.due - now_ < numFrames)
                        finishStage(stage, out, numFrames);
                    for (std::size_t i = 0; i < b; ++i)
                        stage.frame[i] = stage.frame[b + i];
                    stage.fill = 0;
                }
            }
        }

        if (ringSize_ > 0)
            ringPos_ = (ringPos_ + numFrames) % ringSize_;
        now_ += numFrames;
        for (std::size_t i = 0; i < history; ++i)
            input_[i] = input_[numFrames + i];
    }

    // At most this many multiply-accumulate steps per task: enough to spread the work, few enough to stay cheap.
    enum { kMaxMacSteps = 8 };

    // The block just filled `stage.frame`'s second half at absolute time `completed`. Takes the frame into the
    // stage's newest history slot (the task transforms it there, so process() can go on filling the frame), then
    // starts the task, or with no task runs the steps at once.
    void startStage(Stage& stage, std::size_t completed)
    {
        const std::size_t n = 2 * stage.blockSize;
        stage.newest = stage.newest + 1 == stage.count ? 0 : stage.newest + 1;
        // Snapshot the completed frame into `work`, which the forward transform reads (it cannot read
        // stage.frame: processChunk() starts refilling that the moment this returns). The transform's
        // output goes into the newest history slot, claimed just above.
        for (std::size_t i = 0; i < n; ++i)
            stage.work[i] = stage.frame[i];
        // The first output sample is needed D + latency - B samples after the block completes.
        const std::size_t slack = stage.offset + latency_ - stage.blockSize;
        stage.due = completed + slack;
        stage.pending = true;
        if (stage.task != 0)
        {
            // The next block of this stage completes B samples on, and the task must be joined by then.
            const std::size_t deadline = slack < stage.blockSize ? slack : stage.blockSize;
            scheduler_->start(stage.task, static_cast<unsigned>(deadline));
        }
        else
        {
            const std::size_t steps = stage.forwardSteps + stage.macSteps + stage.inverseSteps;
            for (std::size_t step = 0; step < steps; ++step)
                runStep(stage, step);
        }
    }

    // Joins the stage's task and adds its block's output at the time it is needed (relative to this chunk).
    void finishStage(Stage& stage, float* out, std::size_t numFrames)
    {
        if (stage.task != 0)
            scheduler_->join(stage.task);
        stage.pending = false;
        place(stage.work + stage.blockSize, stage.blockSize, stage.due - now_, out, numFrames);
    }

    static void stepTrampoline(void* context, unsigned step)
    {
        Stage& stage = *static_cast<Stage*>(context);
        stage.owner->runStep(stage, step);
    }

    // Overlap-save in steps: the forward transform of the snapshot in `work` into the newest history slot, the sum
    // over the K partitions (newest input spectrum with the first partition, the one before with the second, ...)
    // into `acc`, and the inverse transform of that back into `work`. Leaves the B valid output samples in
    // work[B, 2B).
    //
    // Both transforms are out-of-place (Ne10 requires it). Ne10's c2r normalises by 1/N itself, so the filter
    // spectra built in prepare() are deliberately left unscaled.
    void runStep(Stage& stage, std::size_t step)
    {
        const std::size_t spectrum = RealFft::spectrumFloats(2 * stage.blockSize);
        if (step < stage.forwardSteps)
        {
            stage.fft.forward(stage.work, stage.history + stage.newest * spectrum);
            return;
        }
        step -= stage.forwardSteps;
        if (step < stage.macSteps)
        {
            if (step == 0)
                for (std::size_t i = 0; i < spectrum; ++i)
                    stage.acc[i] = 0.0f;
            const std::size_t first = step * stage.partitionsPerStep;
            const std::size_t last = first + stage.partitionsPerStep < stage.count ? first + stage.partitionsPerStep : stage.count;
            for (std::size_t k = first; k < last; ++k)
            {
                const std::size_t slot = (stage.newest + stage.count - k) % stage.count;
                const float* x = stage.history + slot * spectrum;
                const float* h = stage.filters + k * spectrum;
                for (std::size_t i = 0; i < spectrum; i += 2)
                {
                    stage.acc[i] += x[i] * h[i] - x[i + 1] * h[i + 1];
                    stage.acc[i + 1] += x[i] * h[i + 1] + x[i + 1] * h[i];
                }
            }
            return;
        }
        stage.fft.inverse(stage.acc, stage.work);
    }

    void releaseCfgs()
    {
        for (int i = 0; i < kMaxStages; ++i)
            if (cfgs_[i] != 0)
            {
                ne10_fft_destroy_r2c_float32(cfgs_[i]);
                cfgs_[i] = 0;
            }
    }

    // Adds `count` samples at chunk-relative time `when`: what falls in this chunk goes straight to `out`, the rest
    // to the ring.
    void place(const float* values, std::size_t count, std::size_t when, float* out, std::size_t numFrames)
    {
        std::size_t i = 0;
        for (; i < count && when + i < numFrames; ++i)
            out[when + i] += values[i];
        if (i == count)
            return;
        std::size_t pos = (ringPos_ + when + i) % ringSize_;
        for (; i < count; ++i)
        {
            ring_[pos] += values[i];
            if (++pos == ringSize_)
                pos = 0;
        }
    }

    Stage stages_[kMaxStages];
    std::size_t numStages_;
    std::size_t headTaps_;
    std::size_t latency_;
    std::size_t maxFrames_;
    float* taps_;    // the head, reversed
    float* input_;   // headTaps - 1 samples of history, then the current chunk
    float* ring_;    // stage output scheduled for future chunks, indexed by time
    std::size_t ringSize_;
    std::size_t ringPos_; // the slot for the current chunk's first sample
    float* scratch_; // 2�Bmax + 2: spectra and the stage output
    std::size_t now_; // samples processed since reset(): stages' deadlines are measured on it
    TaskScheduler* scheduler_;
    std::size_t minTaskBlock_;
    bool ready_;
    // One Ne10 configuration per stage, owned here rather than in Stage. A cfg carries internal scratch
    // (cfg->buffer) that a transform writes to, so it can never be shared by two stages running
    // concurrently -- which is exactly what worker threads will do. Keeping ownership in the core also
    // means assigning a fresh Stage over an old one can neither leak nor double-free a cfg.
    ne10_fft_r2c_cfg_float32_t cfgs_[kMaxStages];
};

} // namespace detail

// Uniform partitions of `blockSize` (a power of two) samples. Two flavours:
//
// - zeroLatency = false: every partition is an FFT partition; output is the convolution delayed by exactly
//   blockSize samples. Per sample it costs about 2 FFTs of 2B points / B plus K complex multiplies.
// - zeroLatency = true: the first blockSize taps run directly (blockSize multiply-adds per sample) and the rest in
//   the frequency domain; output is the convolution itself, with no delay.
//
// The work is lumpy: a call in which a block completes does both FFTs and the K-partition sum.
class UniformPartitionedConvolver
{
public:
    UniformPartitionedConvolver() {}

    static std::size_t requiredFloats(std::size_t irLength, std::size_t blockSize, std::size_t maxFrames,
                                      bool zeroLatency = false)
    {
        detail::PartitionedConvolutionCore::Config config;
        if (!configure(irLength, blockSize, zeroLatency, config))
            return 0;
        return detail::PartitionedConvolutionCore::requiredFloats(config, maxFrames);
    }

    // `memory` holds `memoryFloats` >= requiredFloats(...) floats and stays the convolver's until the next prepare().
    // `maxFrames` is the largest chunk process() handles in one pass (the host's maximum block size). Returns false,
    // and outputs silence, if blockSize isn't a power of two, the IR is empty or the memory is too small.
    bool prepare(const float* ir, std::size_t irLength, std::size_t blockSize, float* memory, std::size_t memoryFloats,
                 std::size_t maxFrames, bool zeroLatency = false)
    {
        detail::PartitionedConvolutionCore::Config config;
        if (!configure(irLength, blockSize, zeroLatency, config))
        {
            core_.invalidate();
            return false;
        }
        return core_.prepare(config, ir, irLength, memory, memoryFloats, maxFrames);
    }

    // Before prepare(): stages of at least `minTaskBlock` samples run as background tasks of `scheduler`.
    void setScheduler(TaskScheduler* scheduler, std::size_t minTaskBlock) { core_.setScheduler(scheduler, minTaskBlock); }
    bool ready() const { return core_.ready(); }
    // Samples by which the output trails the convolution: blockSize, or 0 with zeroLatency.
    std::size_t latency() const { return core_.latency(); }
    void reset() { core_.reset(); }
    // Frees the Ne10 configurations and marks this unprepared, so process() outputs silence. Call before
    // releasing the `memory` block handed to prepare(): the stages hold raw pointers into it.
    void invalidate() { core_.invalidate(); }
    // Any numFrames; `in` and `out` may be the same buffer.
    void process(const float* in, float* out, std::size_t numFrames) { core_.process(in, out, numFrames); }

private:
    static bool configure(std::size_t irLength, std::size_t blockSize, bool zeroLatency,
                          detail::PartitionedConvolutionCore::Config& config)
    {
        if (irLength == 0 || !RealFft::isPowerOfTwo(blockSize))
            return false;
        if (zeroLatency)
        {
            config.headTaps = irLength < blockSize ? irLength : blockSize;
            if (irLength > blockSize)
            {
                config.numStages = 1;
                config.blockSizes[0] = blockSize;
                config.blockCounts[0] = detail::ceilDiv(irLength - blockSize, blockSize);
                config.offsets[0] = blockSize;
            }
        }
        else
        {
            config.numStages = 1;
            config.blockSizes[0] = blockSize;
            config.blockCounts[0] = detail::ceilDiv(irLength, blockSize);
            config.offsets[0] = 0;
            config.latency = blockSize;
        }
        return detail::PartitionedConvolutionCore::valid(config);
    }

    detail::PartitionedConvolutionCore core_;
};

// Gardner's non-uniform partitioned convolution, zero latency. The response is split into stages by layout():
// stage 0 (its first K x minBlockSize taps) runs directly; each later stage is K FFT partitions of twice the
// previous block size, up to maxBlockSize, which takes the rest of the response in as many partitions as it needs.
// The output is the convolution itself, sample for sample, whatever the host's buffer size.
//
// Cost: minBlockSize x blocksPerRampStage multiply-adds per sample for the head (512 for the defaults 128 and 4),
// plus each stage's two 2B-point FFTs per B samples and its K-spectrum sum. The heaviest call is the one where
// every stage completes at once (every maxBlockSize samples); keep maxBlockSize at 8192 or 16384 for reverb-length
// responses rather than letting the default grow it to half the response.
class NonUniformPartitionedConvolver
{
public:
    enum { kMaxStages = detail::PartitionedConvolutionCore::kMaxStages };

    // A stage layout: the Python reference's calculate_optimal_nupc_stages, field for field.
    struct Layout
    {
        std::size_t numStages;
        std::size_t blockSizes[kMaxStages];
        std::size_t blockCounts[kMaxStages];
        std::size_t totalPaddedLength; // sum of blockCounts x blockSizes, >= the response's length
        bool valid;                    // false if the layout needed more than kMaxStages stages

        Layout() : numStages(0), totalPaddedLength(0), valid(true)
        {
            for (int i = 0; i < kMaxStages; ++i)
            {
                blockSizes[i] = 0;
                blockCounts[i] = 0;
            }
        }
    };

    NonUniformPartitionedConvolver() {}

    // Ramp up from minBlockSize, doubling, blocksPerRampStage partitions per stage, until the response is covered
    // or the block size reaches maxBlockSize; then one tail stage of maxBlockSize partitions takes what is left.
    static Layout layout(std::size_t irLength, std::size_t minBlockSize, std::size_t maxBlockSize,
                         std::size_t blocksPerRampStage)
    {
        Layout result;
        if (irLength == 0)
            return result;
        std::size_t remaining = irLength;
        std::size_t block = minBlockSize;
        while (remaining > 0 && block < maxBlockSize)
        {
            if (result.numStages == static_cast<std::size_t>(kMaxStages))
            {
                result.valid = false;
                return result;
            }
            const std::size_t capacity = blocksPerRampStage * block;
            result.blockSizes[result.numStages] = block;
            if (remaining <= capacity)
            {
                result.blockCounts[result.numStages++] = detail::ceilDiv(remaining, block);
                remaining = 0;
                break;
            }
            result.blockCounts[result.numStages++] = blocksPerRampStage;
            remaining -= capacity;
            block *= 2;
        }
        if (remaining > 0)
        {
            if (result.numStages == static_cast<std::size_t>(kMaxStages))
            {
                result.valid = false;
                return result;
            }
            result.blockSizes[result.numStages] = maxBlockSize;
            result.blockCounts[result.numStages++] = detail::ceilDiv(remaining, maxBlockSize);
        }
        for (std::size_t i = 0; i < result.numStages; ++i)
            result.totalPaddedLength += result.blockSizes[i] * result.blockCounts[i];
        return result;
    }

    // The reference's default when no maximum is given: the largest power of two below the response's length
    // (2^(bit_length(irLength - 1) - 1)), but never less than minBlockSize.
    static std::size_t defaultMaxBlockSize(std::size_t irLength, std::size_t minBlockSize)
    {
        std::size_t bits = 0;
        for (std::size_t v = irLength > 0 ? irLength - 1 : 0; v != 0; v >>= 1)
            ++bits;
        const std::size_t computed = bits > 0 ? (static_cast<std::size_t>(1) << bits) / 2 : 0;
        std::size_t result = computed > minBlockSize ? computed : minBlockSize;
        return result > 0 ? result : 1;
    }

    // maxBlockSize = 0 means defaultMaxBlockSize(). Returns 0 for a configuration prepare() would refuse.
    static std::size_t requiredFloats(std::size_t irLength, std::size_t minBlockSize, std::size_t maxBlockSize,
                                      std::size_t blocksPerRampStage, std::size_t maxFrames)
    {
        detail::PartitionedConvolutionCore::Config config;
        if (!configure(irLength, minBlockSize, maxBlockSize, blocksPerRampStage, config))
            return 0;
        return detail::PartitionedConvolutionCore::requiredFloats(config, maxFrames);
    }

    // minBlockSize and maxBlockSize must be powers of two with maxBlockSize >= minBlockSize (0 for the default), and
    // blocksPerRampStage >= 2 � with one block per stage a stage would start less than a block into the response
    // and could not keep zero latency (the reference produces wrong output in that case when streamed).
    bool prepare(const float* ir, std::size_t irLength, std::size_t minBlockSize, std::size_t maxBlockSize,
                 std::size_t blocksPerRampStage, float* memory, std::size_t memoryFloats, std::size_t maxFrames)
    {
        detail::PartitionedConvolutionCore::Config config;
        if (!configure(irLength, minBlockSize, maxBlockSize, blocksPerRampStage, config))
        {
            core_.invalidate();
            layout_ = Layout();
            return false;
        }
        layout_ = layout(irLength, minBlockSize, resolveMax(irLength, minBlockSize, maxBlockSize),
                         blocksPerRampStage);
        return core_.prepare(config, ir, irLength, memory, memoryFloats, maxFrames);
    }

    // Before prepare(): stages of at least `minTaskBlock` samples run as background tasks of `scheduler`.
    void setScheduler(TaskScheduler* scheduler, std::size_t minTaskBlock) { core_.setScheduler(scheduler, minTaskBlock); }
    bool ready() const { return core_.ready(); }
    std::size_t latency() const { return 0; }
    const Layout& currentLayout() const { return layout_; }
    void reset() { core_.reset(); }
    // Frees the Ne10 configurations and marks this unprepared, so process() outputs silence. Call before
    // releasing the `memory` block handed to prepare(): the stages hold raw pointers into it.
    void invalidate()
    {
        core_.invalidate();
        layout_ = Layout();
    }

    // Any numFrames; `in` and `out` may be the same buffer.
    void process(const float* in, float* out, std::size_t numFrames) { core_.process(in, out, numFrames); }

private:
    static std::size_t resolveMax(std::size_t irLength, std::size_t minBlockSize, std::size_t maxBlockSize)
    {
        return maxBlockSize == 0 ? defaultMaxBlockSize(irLength, minBlockSize) : maxBlockSize;
    }

    static bool configure(std::size_t irLength, std::size_t minBlockSize, std::size_t maxBlockSize,
                          std::size_t blocksPerRampStage, detail::PartitionedConvolutionCore::Config& config)
    {
        if (irLength == 0 || !RealFft::isPowerOfTwo(minBlockSize) || blocksPerRampStage < 2)
            return false;
        maxBlockSize = resolveMax(irLength, minBlockSize, maxBlockSize);
        if (!RealFft::isPowerOfTwo(maxBlockSize) || maxBlockSize < minBlockSize)
            return false;
        const Layout stages = layout(irLength, minBlockSize, maxBlockSize, blocksPerRampStage);
        if (!stages.valid || stages.numStages == 0)
            return false;
        // Stage 0 is the direct head (only as many taps as the response has); the rest are FFT stages.
        const std::size_t head = stages.blockSizes[0] * stages.blockCounts[0];
        config.headTaps = head < irLength ? head : irLength;
        std::size_t offset = head;
        for (std::size_t i = 1; i < stages.numStages; ++i)
        {
            config.blockSizes[i - 1] = stages.blockSizes[i];
            config.blockCounts[i - 1] = stages.blockCounts[i];
            config.offsets[i - 1] = offset;
            offset += stages.blockSizes[i] * stages.blockCounts[i];
        }
        config.numStages = stages.numStages - 1;
        config.latency = 0;
        return detail::PartitionedConvolutionCore::valid(config);
    }

    detail::PartitionedConvolutionCore core_;
    Layout layout_;
};
} /* namespace arm */
} /* namespace neural */

#endif /* NUPC_ARM_CONVOLUTION_H_ */
