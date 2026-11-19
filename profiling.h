#ifndef _PROFILING_H
#define _PROFILING_H

#include <stdint.h>

#define A4091_PROFILE_MAGIC       0x41345047UL
#define A4091_PROFILE_VERSION     1
#define A4091_PROFILE_ARC_COUNT   4096

struct a4091_profile_arc {
    uint32_t from_pc;
    uint32_t self_pc;
    uint32_t count;
};

struct a4091_profile {
    uint32_t magic;
    uint32_t version;
    uint32_t text_size;
    uint32_t arc_capacity;
    volatile uint32_t arc_count;
    volatile uint32_t dropped;
    volatile uint8_t enabled;
    volatile uint8_t busy;
    uint8_t reserved[2];
    struct a4091_profile_arc arcs[A4091_PROFILE_ARC_COUNT];
};

#ifdef USE_PROFILING
void __attribute__((saveallregs)) mcount(void);
int profiling_init(void);
void profiling_deinit(void);
struct a4091_profile *profiling_data(void);
#endif

#endif /* _PROFILING_H */
