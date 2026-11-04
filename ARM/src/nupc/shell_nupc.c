/*
 * Shell command "nupc": enable/disable the NUPC convolver, run its self-test and report real-time statistics.
 */
#include <stdio.h>
#include <string.h>

#include "shell.h"
#include "nupc_glue.h"

const char shell_help_nupc[] = " [on|off|reset|test]\n"
    "  on / off  enable or bypass the convolver on codec input channel 0\n"
    "  reset     clear the statistics counters\n"
    "  test      run the impulse self-test (turns the engine off; uses the real Ne10 FFT)\n"
    "  (no arg)  print statistics\n";
const char shell_help_summary_nupc[] = "NUPC convolution reverb on/off, self-test and statistics";

static void nupcSelfTest(void)
{
    NUPC_SELFTEST t;
    int rc;

    nupc_set_enabled(false);
    rc = nupc_selftest(&t);
    if (rc != 0) {
        printf("NUPC self-test could not run (%d)\n", rc);
        return;
    }
    {
        const int pass = (t.maxErrServiced < 1e-3f * t.peak) && (t.maxErrStolen < 1e-3f * t.peak);
        printf("NUPC self-test %s: %lu samples, peak %g\n", pass ? "PASS" : "FAIL",
            (unsigned long)t.samples, (double)t.peak);
        printf("  max error %g with background servicing (%lu background steps)\n",
            (double)t.maxErrServiced, (unsigned long)t.backgroundSteps);
        printf("  max error %g with every step stolen by the callback (%lu steps)\n",
            (double)t.maxErrStolen, (unsigned long)t.stolenSteps);
    }
}

void shell_nupc( SHELL_CONTEXT *ctx, int argc, char **argv )
{
    NUPC_STATS s;

    if (argc >= 2) {
        if (strcmp(argv[1], "on") == 0) {
            nupc_set_enabled(true);
        } else if (strcmp(argv[1], "off") == 0) {
            nupc_set_enabled(false);
        } else if (strcmp(argv[1], "reset") == 0) {
            nupc_reset_stats();
        } else if (strcmp(argv[1], "test") == 0) {
            nupcSelfTest();
            return;
        } else {
            printf("usage: nupc [on|off|reset|test]\n");
            return;
        }
    }

    nupc_get_stats(&s);
    printf("NUPC: %s, IR %lu samples, %lu stages, working set %lu KiB, arena %lu/%lu KiB\n",
        s.active ? "enabled" : "disabled",
        (unsigned long)s.irLength, (unsigned long)s.numStages, (unsigned long)s.workingKiB,
        (unsigned long)s.arenaUsedKiB, (unsigned long)s.arenaCapacityKiB);
    printf("  blocks %lu, cycles/block last %lu avg %lu max %lu (budget %lu), over budget %lu\n",
        (unsigned long)s.blocks, (unsigned long)s.cyclesLast, (unsigned long)s.cyclesAvg,
        (unsigned long)s.cyclesMax, (unsigned long)s.budgetCycles, (unsigned long)s.overBudget);
    printf("  steps: background %lu, foreground %lu, stolen by audio callback %lu\n",
        (unsigned long)s.backgroundSteps, (unsigned long)s.foregroundSteps, (unsigned long)s.stolenSteps);
    printf("  deadline misses %lu, restarts %lu, max queued %lu\n",
        (unsigned long)s.misses, (unsigned long)s.restarts, (unsigned long)s.maxQueued);
}
