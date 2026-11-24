#include <exec/memory.h>
#include <proto/exec.h>

#include "profiling.h"

#define NO_PROFILE __attribute__((no_instrument_function))

extern char _stext[];
extern char _etext[];

static struct a4091_profile *profile_data;

static void NO_PROFILE
profile_record(uint32_t from_pc, uint32_t self_pc)
{
    struct a4091_profile *profile = profile_data;
    struct a4091_profile_arc *arc;
    uint32_t slot;
    uint32_t base;
    uint32_t pos;

    if (profile == NULL || !profile->enabled || profile->busy)
        return;

    profile->busy = 1;
    base = (uint32_t)_stext;
    if (from_pc < base || self_pc < base ||
        from_pc >= (uint32_t)_etext || self_pc >= (uint32_t)_etext)
        goto out;

    from_pc -= base;
    self_pc -= base;
    slot = ((from_pc >> 2) ^ (self_pc >> 2));
    slot &= A4091_PROFILE_ARC_COUNT - 1;

    for (pos = 0; pos < A4091_PROFILE_ARC_COUNT; pos++) {
        arc = &profile->arcs[slot];
        if (arc->count == 0) {
            arc->from_pc = from_pc;
            arc->self_pc = self_pc;
            arc->count = 1;
            profile->arc_count++;
            goto out;
        }
        if (arc->from_pc == from_pc && arc->self_pc == self_pc) {
            if (arc->count != UINT32_MAX)
                arc->count++;
            goto out;
        }
        slot = (slot + 1) & (A4091_PROFILE_ARC_COUNT - 1);
    }
    profile->dropped++;

out:
    profile->busy = 0;
}

/* GCC inserts this call before register arguments have been saved. */
void NO_PROFILE __attribute__((saveallregs))
mcount(void)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-address"
    profile_record((uint32_t)__builtin_return_address(1),
                   (uint32_t)__builtin_return_address(0));
#pragma GCC diagnostic pop
}

int NO_PROFILE
profiling_init(void)
{
    struct a4091_profile *profile;

    profile = AllocMem(sizeof (*profile), MEMF_CLEAR | MEMF_PUBLIC);
    if (profile == NULL)
        return (0);

    profile->magic = A4091_PROFILE_MAGIC;
    profile->version = A4091_PROFILE_VERSION;
    profile->text_size = (uint32_t)_etext - (uint32_t)_stext;
    profile->arc_capacity = A4091_PROFILE_ARC_COUNT;
    profile->enabled = 1;
    profile_data = profile;
    return (1);
}

void NO_PROFILE
profiling_deinit(void)
{
    struct a4091_profile *profile = profile_data;

    if (profile == NULL)
        return;

    profile->enabled = 0;
    profile_data = NULL;
    FreeMem(profile, sizeof (*profile));
}

struct a4091_profile *NO_PROFILE
profiling_data(void)
{
    return (profile_data);
}
