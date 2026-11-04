// A TaskScheduler for a bare-metal (or RTOS-hosted) single core with a hard audio callback: no threads are created
// here and nothing ever blocks.
//
// Work is split in two tiers, chosen per task from the deadline the engine passes to start():
//
//   * Foreground tier (deadline < foregroundFrames). Short-deadline stages run from advance()/join() on the audio
//     callback itself, round-robin within a per-callback time budget, exactly like AudioThreadTaskScheduler.
//
//   * Background tier (everything else). start() only queues the task. Whoever has spare time calls
//     serviceBackground() -- a low-priority RTOS task in the test harness, the idle loop of main() in the bare-metal
//     product -- which runs ONE step (one FFT, one MAC chunk) of the queued task with the earliest deadline and
//     returns. The audio callback preempts it freely; join() on the callback finishes whatever is left, so the output
//     is always correct and a slow background context only costs callback time ("stolen" steps), never correctness.
//
// Concurrency model (one core, preemption only -- the callback may interrupt serviceBackground() at any point, never
// the reverse):
//   state  Idle -> Queued -> Running -> (Queued | Done) -> Idle
//   Running is held for the duration of ONE step only. The only way join() can find a task Running is that the
//   callback preempted the background context inside that very step; the callback cannot wait for it (it would
//   deadlock the core), so it counts a deadline miss and carries on -- the block's contribution for that stage is
//   stale for this callback. That window is one step (at most one FFT) wide.
//
// 32-bit lock-free atomics only (ldrex/strex on the Cortex-A5).
#ifndef NUPC_BARE_METAL_TASK_SCHEDULER_HPP
#define NUPC_BARE_METAL_TASK_SCHEDULER_HPP

#include "Convolution.h"
#include <atomic>
#include <cstdint>

namespace neural {
namespace arm {
namespace detail {

class BareMetalTaskScheduler : public neural::arm::TaskScheduler
{
public:
    enum { kMaxTasks = 16 };

    struct Counters
    {
        std::uint32_t backgroundSteps;  // steps run by serviceBackground()
        std::uint32_t foregroundSteps;  // steps run from advance() (foreground tier, within the budget)
        std::uint32_t stolenSteps;      // steps the callback had to run itself in join() (background was too slow)
        std::uint32_t misses;           // join() found the task mid-step in the preempted background context
        std::uint32_t restarts;         // start() hit a Running task and queued a restart
        std::uint32_t maxQueued;        // deepest background queue seen
    };

    BareMetalTaskScheduler();

    // Foreground time budget per advance() call, in nanoseconds (0 = foreground steps only run from join()).
    void setBudgetNanos(std::uint64_t nanos) { budgetNanos_ = nanos; }
    // Tasks whose deadline is below this many frames are foreground tasks.
    void setForegroundFrames(unsigned frames) { foregroundFrames_ = frames; }
    // Frames per advance() call (the audio chunk size); used to turn frame deadlines into a callback count.
    void setFramesPerCallback(unsigned frames) { framesPerCallback_ = frames ? frames : 1; }

    // Forgets every task so the owning convolver can be re-prepared. Not legal while another context can be inside
    // serviceBackground().
    void clear();

    // Runs at most `maxSteps` background steps (one task step each, earliest deadline first) and returns how many ran.
    // Call from the idle/background context only, never from the audio callback. Safe to be preempted by it.
    unsigned serviceBackground(unsigned maxSteps = 1);

    // True when no background task is waiting (all queued work has run).
    bool backgroundIdle() const;

    Counters counters() const;
    void resetCounters();

    unsigned create(StepFn fn, void* context, unsigned steps) override;
    void start(unsigned task, unsigned deadlineFrames) override;
    void join(unsigned task) override;
    void advance() override;

private:
    enum State : std::uint32_t { kIdle = 0, kQueued = 1, kRunning = 2, kDone = 3, kForeground = 4 };

    struct Task
    {
        StepFn fn;
        void* context;
        unsigned totalSteps;
        std::atomic<unsigned> nextStep;
        std::atomic<std::uint32_t> state;
        std::atomic<std::uint32_t> restart;
        std::atomic<std::uint32_t> due;  // callback index by which the task must be joined (EDF key)
        std::atomic<bool> foreground;
    };

    // Runs one step of a task the caller has claimed (state == Running) and releases it again.
    void runClaimedStep(Task& t);

    Task tasks_[kMaxTasks];
    unsigned count_;
    unsigned cursor_;
    std::atomic<std::uint32_t> callbackIndex_;
    unsigned foregroundFrames_;
    unsigned framesPerCallback_;
    std::uint64_t budgetNanos_;

    std::atomic<std::uint32_t> backgroundSteps_, foregroundSteps_, stolenSteps_, misses_, restarts_, maxQueued_;
};

} // namespace detail
} // namespace arm
} // namespace neural

#endif // NUPC_BARE_METAL_TASK_SCHEDULER_HPP
