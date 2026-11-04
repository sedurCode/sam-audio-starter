//
// Created by sedur on 12/10/2026.
//

#define NUPC_DEFINE_IR_TABLE // this translation unit owns the one copy of the embedded impulse response
#include "NeuralNonUniformPartitionedConvolution.h"

namespace neural {
    namespace arm {
        namespace detail {
            unsigned AudioThreadTaskScheduler::create(StepFn fn, void* context, unsigned steps)
            {
                if (count_ >= kMaxTasks)
                    return 0;
                Task& t = tasks_[count_];
                t.fn = fn;
                t.context = context;
                t.totalSteps = steps;
                t.nextStep = steps; // idle (nothing left to run) until start()
                ++count_;
                return count_; // 1-based handle; 0 means "no room"
            }

            void AudioThreadTaskScheduler::start(unsigned task, unsigned /*deadlineFrames*/)
            {
                // deadlineFrames isn't used to pick a per-task budget: advance()'s cycle budget already bounds every
                // callback's total background-work cost, and join() is the real deadline backstop in all cases, so
                // there's nothing to gain from tracking per-task deadlines here too.
                tasks_[task - 1].nextStep = 0;
            }

            void AudioThreadTaskScheduler::join(unsigned task)
            {
                Task& t = tasks_[task - 1];
                while (t.nextStep < t.totalSteps)
                    t.fn(t.context, t.nextStep++);
            }

            // Called once per audio buffer from PartitionedConvolutionCore::processChunk(). Runs steps round-robin across
            // pending tasks until either none are left or the cycle budget is spent (see class comment).
            void AudioThreadTaskScheduler::advance()
            {
                if (count_ == 0)
                    return;

                const std::uint64_t start = readNanos();
                unsigned i = cursor_ < count_ ? cursor_ : 0;
                unsigned idleStreak = 0; // consecutive tasks visited with nothing to do; count_ in a row means "all idle"
                while (idleStreak < count_)
                {
                    Task& t = tasks_[i];
                    if (t.nextStep < t.totalSteps)
                    {
                        t.fn(t.context, t.nextStep++);
                        idleStreak = 0;
                        // Checked *after* running a step, deliberately: a step is atomic (can't be paused partway), so
                        // the earliest useful moment to stop is right after one finishes. This can overshoot the budget
                        // by at most one step's cost -- see class comment for why that overshoot is itself bounded.
                        if (static_cast<std::uint64_t>(readNanos() - start) >= budgetNanos_)
                        {
                            i = (i + 1) % count_;
                            break;
                        }
                    }
                    else
                    {
                        ++idleStreak;
                    }
                    i = (i + 1) % count_;
                }
                cursor_ = i;
            }

        }
    }
}


bool NupcConvolver::prepare(float sampleRate)
{
    return prepare(sampleRate, Config());
}

bool NupcConvolver::prepare(float sampleRate, const Config& config)
{
    // A re-prepare must free the previous block before carving a new one, and must leave the object inactive
    // (so a concurrent process() passes through) if anything below fails.
    releaseMemory();

    if (sampleRate <= 0.0f)
        return false;

    // How much of the embedded York Minster response (see york_minster_ir.h) to use: all of it by default, or the
    // requested prefix. No rounding to a power of two here -- the partition layout copes with any length, and the
    // caller is the one who knows whether an exact fit matters.
    std::size_t wanted = config.irLength == 0 ? static_cast<std::size_t>(YORK_MINSTER_IR_LENGTH) : config.irLength;
    if (wanted > YORK_MINSTER_IR_LENGTH)
        wanted = YORK_MINSTER_IR_LENGTH;

    // NOTE: the embedded response is at YORK_MINSTER_IR_SAMPLE_RATE (48 kHz) and is used as-is. If the engine
    // ever runs at another rate the response needs resampling here -- at present `sampleRate` is only used for
    // the scheduler's per-buffer time budget below.

    // Scratch conversion buffer: NonUniformPartitionedConvolver::prepare() (below) reads `ir` fully and FFTs
    // it into its own `memory` block synchronously before returning, so this buffer only needs to live for
    // the duration of this function -- it is not part of the convolver's steady-state footprint.
#ifdef NUPC_BARE_METAL
    // No heap: the conversion buffer is scratch space at the top of the NUPC arena, freed again before returning.
    float* irSamples = static_cast<float*>(nupc_arena_scratch(wanted * sizeof(float)));
#define NUPC_RELEASE_IR_SCRATCH() nupc_arena_scratch_release_all()
#else
    float* irSamples = new float[wanted];
#define NUPC_RELEASE_IR_SCRATCH() delete[] irSamples
#endif
    if (irSamples == 0)
        return false;
    const std::size_t length =
        neural::arm::detail::loadIrSamplePrefix(york_minster_ir_bits, YORK_MINSTER_IR_LENGTH, irSamples, wanted);
    if (length == 0)
    {
        NUPC_RELEASE_IR_SCRATCH();
        return false;
    }

    // requiredFloats() returns 0 for a configuration prepare() would refuse (bad block sizes etc.).
    const std::size_t floats = neural::arm::NonUniformPartitionedConvolver::requiredFloats(
        length, config.minBlock, config.maxBlock, config.blocksPerStage, BUFFER_SIZE_SAMPLES);
    if (floats == 0)
    {
        NUPC_RELEASE_IR_SCRATCH();
        return false;
    }

#ifdef NUPC_BARE_METAL
    float* memory = static_cast<float*>(nupc_arena_alloc(floats * sizeof(float)));
#else
    float* memory = new float[floats];
#endif
    if (memory == 0)
    {
        NUPC_RELEASE_IR_SCRATCH();
        return false;
    }
    memory_ = memory; // owned from here on; releaseMemory() frees it

    // Mono IR (see class comment): the same converted samples feed both channels.
    irScale_ = unitEnergyScale(irSamples, length);

    // Which scheduler runs the background FFT work. Auto: threaded if the machine has a spare core to put a
    // worker on, otherwise spread the steps over audio callbacks. Deciding here (rather than at compile time)
    // means the same binary is correct on this 4-core i.MX8MP and on a 6-core i.MX95.
    unsigned workers = 0;
    neural::arm::TaskScheduler* scheduler;
#ifdef NUPC_BARE_METAL
    // Bare metal: kSchedulerAudioThread keeps everything on the callback (AudioThreadTaskScheduler, as on Linux
    // with one core); anything else uses the foreground/background BareMetalTaskScheduler. `workers` is unused.
    if (config.scheduler != kSchedulerAudioThread)
    {
        const double nanosPerBuffer = 1e9 * (static_cast<double>(BUFFER_SIZE_SAMPLES) / sampleRate);
        bmTasks_.clear();
        bmTasks_.setFramesPerCallback(BUFFER_SIZE_SAMPLES);
        bmTasks_.setForegroundFrames(static_cast<unsigned>(config.foregroundFrames));
        bmTasks_.setBudgetNanos(static_cast<std::uint64_t>(nanosPerBuffer * kBackgroundBudgetFraction));
        bmTasks_.resetCounters();
        scheduler = &bmTasks_;
    }
    else
#else
    if (config.scheduler != kSchedulerAudioThread)
        workers = config.workers != 0 ? config.workers : neural::arm::detail::ThreadedTaskScheduler::defaultWorkerCount();
    if (workers > 0)
    {
        scheduler = &threadedTasks_;
    }
    else
#endif
    {
        // Time budget for the background-FFT scheduler (see AudioThreadTaskScheduler's class comment): up to
        // kBackgroundBudgetFraction of one audio buffer's wall-clock time. No initialisation or clock-rate query
        // is needed -- steady_clock is already in real time units (see detail::readNanos()).
        const double nanosPerBuffer = 1e9 * (static_cast<double>(BUFFER_SIZE_SAMPLES) / sampleRate);
        tasks_.setBudgetNanos(static_cast<std::uint64_t>(nanosPerBuffer * kBackgroundBudgetFraction));
        scheduler = &tasks_;
    }

    // NOTE: convolver_/memory still need to become genuine 2-element (stereo) arrays below -- that wiring is
    // pre-existing/unrelated to the IR-loading change above and is left as-is for now (see the mono bring-up
    // wiring in ARMAudioEngine::processIMX8MP()).
    convolver_.setScheduler(scheduler, config.minTaskBlock);
    if (!convolver_.prepare(irSamples, length,
            config.minBlock, config.maxBlock, config.blocksPerStage,
            memory_, floats,
            BUFFER_SIZE_SAMPLES))
    {
        NUPC_RELEASE_IR_SCRATCH();
        releaseMemory();
        return false;
    }
    NUPC_RELEASE_IR_SCRATCH();
    reset();
    config_ = config;
    loadedIrLength_ = length;
    memoryFloats_ = floats;
#ifdef NUPC_BARE_METAL
    usingThreads_ = false;
#else
    threadedTasks_.resetCounters();
    usingThreads_ = workers > 0;
    // Last, once every task exists and the convolver's state is clean: let the workers loose. Nothing they can
    // pick up has been queued yet -- that only happens from process() -- so there is no race with reset().
    if (workers > 0)
        threadedTasks_.startWorkers(workers);
#endif
    active_ = true;
    return true;
}

NupcConvolver::Stats NupcConvolver::stats() const
{
    Stats st;
    st.active = active_;
    if (!active_)
        return st;
    const auto& layout = convolver_.currentLayout();
    st.irLength = loadedIrLength_;
    st.minBlock = config_.minBlock;
    st.maxBlock = config_.maxBlock;
    st.blocksPerStage = config_.blocksPerStage;
    st.minTaskBlock = config_.minTaskBlock;
    st.numStages = layout.numStages;
    st.paddedLength = layout.totalPaddedLength;
    st.workingFloats = memoryFloats_;
    st.threaded = usingThreads_;
    st.irScale = irScale_;
#ifdef NUPC_BARE_METAL
    const neural::arm::detail::BareMetalTaskScheduler::Counters c = bmTasks_.counters();
    st.stolenBack = c.stolenSteps;
    st.blockedOnWorker = c.misses;
    st.workerTasks = c.backgroundSteps;
#else
    st.workers = threadedTasks_.workerCount();
    st.stolenBack = threadedTasks_.stolenBack();
    st.blockedOnWorker = threadedTasks_.blockedOnWorker();
    st.workerBusyNanos = threadedTasks_.workerBusyNanos();
    st.workerTasks = threadedTasks_.workerTasks();
#endif
    return st;
}

void NupcConvolver::reset()
{
    convolver_.reset();
//        mixSmoothed_.setImmediate(mixSmoothed_.target());
//        gainSmoothed_.setImmediate(gainSmoothed_.target());
}

void NupcConvolver::process(const float* in, float* out)
{
    if (!active_)
    {
        // Passthrough, so the caller can wire this in unconditionally (the previous early-return left `out`
        // holding whatever stale content it had, which for a path that writes nothing else means silence).
        if (out != in)
            std::memcpy(out, in, BUFFER_SIZE_SAMPLES * sizeof(float));
        return;
    }

    // The convolver cannot work in place against `out` here, because the mix below needs the dry input and the
    // wet result at the same time -- and `in` may alias `out`.
    float wetBuffer[BUFFER_SIZE_SAMPLES];
    convolver_.process(in, wetBuffer, BUFFER_SIZE_SAMPLES);

    // Hoisted out of the loop: these were per-sample, which meant a std::pow() call per sample.
    const float scale = normalise_ ? irScale_ : 1.0f;
    const float mix = mix_;
    const float wetGain = mix * linearGain() * scale;
    const float dryGain = 1.0f - mix;

    for (uint32_t i = 0; i < BUFFER_SIZE_SAMPLES; ++i)
        out[i] = dryGain * in[i] + wetGain * wetBuffer[i];
}

//    void setParam(uint32_t index, float value)
//    {
//        if (index == param_mix)
//        {
//            mix_ = value;
//            mixSmoothed_.setTarget(value);
//        }
//        else if (index == param_gain)
//        {
//            gainDb_ = value;
//            gainSmoothed_.setTarget(linearGain());
//        }
//        else if (index == param_normalise)
//            normalise_ = value >= 0.5f;
//    }
