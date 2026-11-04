#include "BareMetalTaskScheduler.hpp"
#include "NupcPlatform.h"

namespace neural {
namespace arm {
namespace detail {

namespace {
    inline bool dueBefore(std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a - b) < 0; }
}

BareMetalTaskScheduler::BareMetalTaskScheduler()
    : count_(0), cursor_(0), callbackIndex_(0), foregroundFrames_(512), framesPerCallback_(1), budgetNanos_(0),
      backgroundSteps_(0), foregroundSteps_(0), stolenSteps_(0), misses_(0), restarts_(0), maxQueued_(0)
{
    for (unsigned i = 0; i < kMaxTasks; ++i)
    {
        tasks_[i].fn = 0;
        tasks_[i].context = 0;
        tasks_[i].totalSteps = 0;
        tasks_[i].nextStep.store(0);
        tasks_[i].state.store(kIdle);
        tasks_[i].restart.store(0);
        tasks_[i].due.store(0);
        tasks_[i].foreground.store(false);
    }
}

void BareMetalTaskScheduler::clear()
{
    count_ = 0;
    cursor_ = 0;
    for (unsigned i = 0; i < kMaxTasks; ++i)
    {
        tasks_[i].state.store(kIdle);
        tasks_[i].restart.store(0);
        tasks_[i].nextStep.store(0);
    }
}

BareMetalTaskScheduler::Counters BareMetalTaskScheduler::counters() const
{
    Counters c;
    c.backgroundSteps = backgroundSteps_.load();
    c.foregroundSteps = foregroundSteps_.load();
    c.stolenSteps = stolenSteps_.load();
    c.misses = misses_.load();
    c.restarts = restarts_.load();
    c.maxQueued = maxQueued_.load();
    return c;
}

void BareMetalTaskScheduler::resetCounters()
{
    backgroundSteps_.store(0);
    foregroundSteps_.store(0);
    stolenSteps_.store(0);
    misses_.store(0);
    restarts_.store(0);
    maxQueued_.store(0);
}

unsigned BareMetalTaskScheduler::create(StepFn fn, void* context, unsigned steps)
{
    if (count_ >= kMaxTasks)
        return 0;
    Task& t = tasks_[count_];
    t.fn = fn;
    t.context = context;
    t.totalSteps = steps;
    t.nextStep.store(steps);   // nothing to run until start()
    t.state.store(kIdle);
    t.restart.store(0);
    t.foreground.store(false, std::memory_order_relaxed);
    ++count_;
    return count_;             // 1-based handle; 0 means "no room"
}

void BareMetalTaskScheduler::start(unsigned task, unsigned deadlineFrames)
{
    Task& t = tasks_[task - 1];

    if (deadlineFrames < foregroundFrames_)
    {
        // Foreground task: only the audio callback ever touches it.
        t.foreground.store(true, std::memory_order_relaxed);
        t.nextStep.store(0);
        t.state.store(kForeground);
        return;
    }

    t.foreground.store(false, std::memory_order_relaxed);
    const unsigned callbacks = (deadlineFrames + framesPerCallback_ - 1) / framesPerCallback_;
    const std::uint32_t due = callbackIndex_.load() + (callbacks ? callbacks : 1);

    for (;;)
    {
        std::uint32_t s = t.state.load(std::memory_order_acquire);
        if (s == kRunning)
        {
            // The background context was preempted inside a step of the previous block. It cannot be restarted from
            // here; ask it to start over when that step returns.
            t.restart.store(1);
            restarts_.fetch_add(1);
            return;
        }
        t.due.store(due, std::memory_order_relaxed);
        t.nextStep.store(0, std::memory_order_relaxed);
        if (t.state.compare_exchange_strong(s, kQueued, std::memory_order_acq_rel))
            break;
        // The background context claimed the task between the load and the exchange: look again.
    }

    unsigned queued = 0;
    for (unsigned i = 0; i < count_; ++i)
        if (tasks_[i].state.load(std::memory_order_relaxed) == kQueued)
            ++queued;
    if (queued > maxQueued_.load(std::memory_order_relaxed))
        maxQueued_.store(queued, std::memory_order_relaxed);
}

void BareMetalTaskScheduler::join(unsigned task)
{
    Task& t = tasks_[task - 1];

    if (t.foreground.load(std::memory_order_relaxed))
    {
        for (unsigned n = t.nextStep.load(); n < t.totalSteps; n = t.nextStep.load())
        {
            t.fn(t.context, n);
            t.nextStep.store(n + 1);
        }
        t.state.store(kIdle);
        return;
    }

    for (;;)
    {
        std::uint32_t s = t.state.load(std::memory_order_acquire);
        switch (s)
        {
        case kIdle:
            return;
        case kDone:
            t.state.store(kIdle, std::memory_order_release);
            return;
        case kQueued:
            if (t.state.compare_exchange_strong(s, kRunning, std::memory_order_acq_rel))
            {
                // Nobody had run (all of) it: the callback finishes it here.
                for (unsigned n = t.nextStep.load(); n < t.totalSteps; n = t.nextStep.load())
                {
                    t.fn(t.context, n);
                    t.nextStep.store(n + 1, std::memory_order_release);
                    stolenSteps_.fetch_add(1);
                }
                t.restart.store(0);
                t.state.store(kIdle, std::memory_order_release);
                return;
            }
            break; // lost a race with the background context: re-read the state
        case kRunning:
            // The preempted background context is inside a step of this very task. All steps already finished means
            // only its bookkeeping is pending, which is harmless; otherwise this block is late.
            if (t.nextStep.load(std::memory_order_acquire) < t.totalSteps)
                misses_.fetch_add(1);
            return;
        default:
            return;
        }
    }
}

void BareMetalTaskScheduler::advance()
{
    callbackIndex_.fetch_add(1);

    if (budgetNanos_ == 0 || count_ == 0)
        return;

    const std::uint64_t start = nupc_read_nanos();
    unsigned i = cursor_ < count_ ? cursor_ : 0;
    unsigned idleStreak = 0;
    while (idleStreak < count_)
    {
        Task& t = tasks_[i];
        const unsigned n = t.nextStep.load();
        if (t.foreground.load(std::memory_order_relaxed) && t.state.load() == kForeground && n < t.totalSteps)
        {
            t.fn(t.context, n);
            t.nextStep.store(n + 1);
            foregroundSteps_.fetch_add(1);
            idleStreak = 0;
            if (nupc_read_nanos() - start >= budgetNanos_)
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

unsigned BareMetalTaskScheduler::serviceBackground(unsigned maxSteps)
{
    unsigned ran = 0;
    while (ran < maxSteps)
    {
        // Earliest-deadline queued task. The scan is racy against the callback by design: the claim below decides.
        int best = -1;
        for (unsigned i = 0; i < count_; ++i)
        {
            const Task& t = tasks_[i];
            if (t.foreground.load(std::memory_order_relaxed) || t.state.load(std::memory_order_acquire) != kQueued)
                continue;
            if (best < 0 || dueBefore(t.due.load(std::memory_order_relaxed), tasks_[best].due.load(std::memory_order_relaxed)))
                best = static_cast<int>(i);
        }
        if (best < 0)
            break;

        Task& t = tasks_[best];
        std::uint32_t expected = kQueued;
        if (!t.state.compare_exchange_strong(expected, kRunning, std::memory_order_acq_rel))
            continue; // the callback stole it; pick again

        // The callback may preempt us anywhere in here. While state is Running it will not touch the task's steps.
        const unsigned n = t.nextStep.load(std::memory_order_acquire);
        if (n < t.totalSteps)
        {
            t.fn(t.context, n);
            t.nextStep.store(n + 1, std::memory_order_release);
            backgroundSteps_.fetch_add(1);
            ++ran;
        }

        if (t.restart.exchange(0) != 0)
        {
            t.nextStep.store(0, std::memory_order_relaxed);
            t.state.store(kQueued, std::memory_order_release);
        }
        else if (t.nextStep.load(std::memory_order_relaxed) >= t.totalSteps)
        {
            t.state.store(kDone, std::memory_order_release);
        }
        else
        {
            t.state.store(kQueued, std::memory_order_release);
        }
    }
    return ran;
}

bool BareMetalTaskScheduler::backgroundIdle() const
{
    for (unsigned i = 0; i < count_; ++i)
    {
        const std::uint32_t s = tasks_[i].state.load(std::memory_order_acquire);
        if (s == kQueued || s == kRunning)
            return false;
    }
    return true;
}

} // namespace detail
} // namespace arm
} // namespace neural
