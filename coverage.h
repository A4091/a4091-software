#ifndef _COVERAGE_H
#define _COVERAGE_H

#include <stdint.h>

#define A4091_COVERAGE_MAGIC   0x41344356UL
#define A4091_COVERAGE_VERSION 1

struct a4091_coverage {
    uint32_t magic;
    uint32_t version;
    uint32_t total_size;
    uint32_t file_count;
    uint8_t data[];
};

struct a4091_coverage_file {
    uint32_t record_size;
    uint32_t name_size;
    uint32_t data_size;
    uint8_t data[];
};

#ifdef USE_COVERAGE
int coverage_snapshot(void);
void coverage_deinit(void);
struct a4091_coverage *coverage_data(void);
#endif

#endif /* _COVERAGE_H */
