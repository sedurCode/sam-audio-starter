// Bare-metal platform layer for NUPC: arena allocator, cycle-counter clock, FPSCR helper. See NupcPlatform.h.
#include "NupcPlatform.h"
#ifndef __arm__
#include <chrono>
#endif

#ifdef NUPC_BARE_METAL

// ---- Arena --------------------------------------------------------------------------------------------------------
namespace {
    // Default arena: placed in DDR (.bss -> MEM_L3 in the starter's linker script). Large enough for a few seconds of
    // impulse response; raise NUPC_ARENA_BYTES (CMake cache variable SAM_NUPC_ARENA_MB) for longer IRs.
#ifndef NUPC_ARENA_BYTES
#define NUPC_ARENA_BYTES (16u * 1024u * 1024u)
#endif
    alignas(64) uint8_t g_defaultArena[NUPC_ARENA_BYTES];

    uint8_t* g_base = g_defaultArena;
    size_t   g_size = sizeof(g_defaultArena);
    size_t   g_bottom = 0;      // persistent allocations occupy [0, g_bottom)
    size_t   g_top = sizeof(g_defaultArena); // scratch allocations occupy [g_top, g_size)

    inline size_t align16(size_t n) { return (n + 15u) & ~static_cast<size_t>(15u); }
}

extern "C" {

void nupc_arena_init(void* base, size_t bytes)
{
    uintptr_t b = (reinterpret_cast<uintptr_t>(base) + 15u) & ~static_cast<uintptr_t>(15u);
    g_base = reinterpret_cast<uint8_t*>(b);
    g_size = bytes - (b - reinterpret_cast<uintptr_t>(base));
    g_bottom = 0;
    g_top = g_size;
}

void* nupc_arena_alloc(size_t bytes)
{
    const size_t n = align16(bytes);
    if (n > g_top - g_bottom)
        return 0;
    void* p = g_base + g_bottom;
    g_bottom += n;
    return p;
}

void nupc_arena_free(void*) { /* freed all at once by nupc_arena_reset() */ }

void nupc_arena_reset(void) { g_bottom = 0; }

void* nupc_arena_scratch(size_t bytes)
{
    const size_t n = align16(bytes);
    if (n > g_top - g_bottom)
        return 0;
    g_top -= n;
    return g_base + g_top;
}

void nupc_arena_scratch_release_all(void) { g_top = g_size; }

size_t nupc_arena_used(void) { return g_bottom + (g_size - g_top); }
size_t nupc_arena_capacity(void) { return g_size; }

} // extern "C"

// ---- Clock --------------------------------------------------------------------------------------------------------
namespace {
    uint32_t g_cpuHz = 500000000u;
    volatile uint32_t g_hi = 0;   // wrap count of the 32-bit cycle counter
    volatile uint32_t g_last = 0;

    // Save CPSR and mask IRQs: nupc_read_nanos() runs from both the audio ISR and the background task on one core.
#ifdef __arm__
    inline uint32_t irqSave()
    {
        uint32_t cpsr;
        __asm__ volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr) : : "memory", "cc");
        return cpsr;
    }
    inline void irqRestore(uint32_t cpsr)
    {
        if (!(cpsr & 0x80u))
            __asm__ volatile("cpsie i" : : : "memory");
    }
#else
    // Host build (unit tests): the clock is serialised by a plain mutex-free assumption of one reader at a time.
    inline uint32_t irqSave() { return 0; }
    inline void irqRestore(uint32_t) {}
#endif
}

extern "C" {

void nupc_clock_init(uint32_t cpuHz)
{
    g_cpuHz = cpuHz ? cpuHz : g_cpuHz;
#ifdef __arm__
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(v));            // PMCR
    v |= 0x7u;                                                           // E | P | C: enable, reset counters
    v &= ~(1u << 3);                                                     // D = 0: count every cycle
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 0" : : "r"(v));
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 1" : : "r"(0x80000000u)); // PMCNTENSET: cycle counter
    __asm__ volatile("isb");
#endif
    g_hi = 0;
    g_last = nupc_read_cycles();
}

uint32_t nupc_read_cycles(void)
{
#ifdef __arm__
    uint32_t c;
    __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(c));            // PMCCNTR
    return c;
#else
    // Host build (unit tests): steady clock expressed as cycles of the configured core clock.
    const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    return static_cast<uint32_t>(ns * (g_cpuHz / 1000000u) / 1000u);
#endif
}

uint64_t nupc_read_nanos(void)
{
    const uint32_t s = irqSave();
    const uint32_t c = nupc_read_cycles();
    if (c < g_last)
        ++g_hi;
    g_last = c;
    const uint64_t cycles = (static_cast<uint64_t>(g_hi) << 32) | c;
    irqRestore(s);
    // cycles * 1e9 / hz without overflowing 64 bits for ~58 years at 500 MHz: split into whole seconds + remainder.
    const uint64_t secs = cycles / g_cpuHz;
    const uint64_t rem = cycles - secs * g_cpuHz;
    return secs * 1000000000ull + (rem * 1000000000ull) / g_cpuHz;
}

void nupc_enable_flush_to_zero(void)
{
#ifdef __arm__
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24);                                                 // FZ
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
#endif
}

} // extern "C"

#endif // NUPC_BARE_METAL
