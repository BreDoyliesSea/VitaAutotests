// What probe_module's start and stop functions record, through a pointer in their arguments
#pragma once

#include <psp2/types.h>

typedef struct ModuleProbe {
    SceUID thread;
    SceUID process;
    int priority;
    int init_priority;
    int stack_size;
    unsigned int attr;
    SceUIntPtr frame;
    SceUIntPtr argp;
    SceSize args;
    int runs;
} ModuleProbe;

typedef struct ModuleArgs {
    ModuleProbe *out;
} ModuleArgs;
