/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_flags.h"

DEFINE_bool(headless, false,
            "Don't display any UI, using defaults for prompts as needed.",
            "UI");
DEFINE_bool(log_high_frequency_kernel_calls, false,
            "Log kernel calls with the kHighFrequency tag.", "Logging");
DEFINE_bool(
    guest_scheduler, false,
    "Run guest threads as cooperative fibers driven by an in-kernel scheduler "
    "instead of mapping each to its own host OS thread. Experimental; off by "
    "default. Requires a restart to take effect.",
    "Kernel");
DEFINE_uint32(
    guest_scheduler_cpus, 6,
    "Number of host dispatch threads the cooperative scheduler spreads the "
    "360's 6 logical CPUs across (clamped to 1-6). 6 is one per guest CPU "
    "(full "
    "parallelism), 3 is one per physical core (SMT pairs share a thread), 1 is "
    "all guest threads cooperative on a single thread. Requires a restart.",
    "Kernel");
