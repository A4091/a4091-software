#include <stddef.h>
#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <gcov.h>

#include "coverage.h"

#define NO_COVERAGE __attribute__((no_profile_instrument_function))

extern const struct gcov_info *const coverage_info_start[];
extern const struct gcov_info *const coverage_info_end[];

struct coverage_allocation {
    struct coverage_allocation *next;
    uint32_t size;
    uint8_t data[];
};

struct coverage_stream {
    struct a4091_coverage *coverage;
    struct a4091_coverage_file *file;
    uint32_t offset;
    uint32_t files;
    int failed;
};

static struct a4091_coverage *coverage_current;
static struct coverage_allocation *coverage_allocations;

extern void panic(const char *s, ...);
void *mmap(void);
void abort(void) __attribute__((noreturn));
void __gcov_merge_add(void);

/*
 * libgcov first tries mmap() for internal scratch space. The driver has no
 * mmap implementation, so force it to use the supplied allocation callback.
 */
void *NO_COVERAGE
mmap(void)
{
    return (NULL);
}

void NO_COVERAGE __attribute__((noreturn))
abort(void)
{
    panic("libgcov aborted");
    for (;;)
        ;
}

/* Referenced by gcov_info records but unused while serializing counters. */
void NO_COVERAGE
__gcov_merge_add(void)
{
}

static void *NO_COVERAGE
coverage_allocate(unsigned int length, void *arg)
{
    struct coverage_allocation *allocation;

    (void)arg;
    allocation = AllocMem(sizeof (*allocation) + length,
                          MEMF_CLEAR | MEMF_PUBLIC);
    if (allocation == NULL)
        return (NULL);

    allocation->size = sizeof (*allocation) + length;
    allocation->next = coverage_allocations;
    coverage_allocations = allocation;
    return (allocation->data);
}

static int NO_COVERAGE
coverage_room(struct coverage_stream *stream, uint32_t length)
{
    if (stream->failed)
        return (0);
    if (length > UINT32_MAX - stream->offset ||
        (stream->coverage != NULL &&
         (stream->offset > stream->coverage->total_size ||
          length > stream->coverage->total_size - stream->offset))) {
        stream->failed = 1;
        return (0);
    }
    return (1);
}

static void NO_COVERAGE
coverage_filename(const char *filename, void *arg)
{
    struct coverage_stream *stream = arg;
    struct a4091_coverage_file *file;
    uint32_t name_size;

    if (filename == NULL)
        filename = "unknown.gcda";
    name_size = strlen(filename) + 1;
    if (name_size > UINT32_MAX - sizeof (*file)) {
        stream->failed = 1;
        return;
    }
    if (!coverage_room(stream, sizeof (*file) + name_size))
        return;

    if (stream->coverage == NULL) {
        stream->offset += sizeof (*file) + name_size;
        stream->files++;
        return;
    }

    file = (void *)((uint8_t *)stream->coverage + stream->offset);
    file->name_size = name_size;
    file->data_size = 0;
    CopyMem(filename, file->data, name_size);
    stream->offset += sizeof (*file) + name_size;
    stream->file = file;
    stream->files++;
}

static void NO_COVERAGE
coverage_dump(const void *data, unsigned int length, void *arg)
{
    struct coverage_stream *stream = arg;

    if (!coverage_room(stream, length))
        return;
    if (stream->coverage != NULL) {
        CopyMem(data, (uint8_t *)stream->coverage + stream->offset, length);
        stream->file->data_size += length;
    }
    stream->offset += length;
}

static void NO_COVERAGE
coverage_serialize(struct coverage_stream *stream)
{
    const struct gcov_info *const *info;
    uint32_t padding;

    info = coverage_info_start;
    while (info != coverage_info_end) {
        stream->file = NULL;
        __gcov_info_to_gcda(*info, coverage_filename, coverage_dump,
                            coverage_allocate, stream);
        padding = (-stream->offset) & 3;
        if (!coverage_room(stream, padding))
            return;
        stream->offset += padding;
        if (stream->coverage != NULL) {
            stream->file->record_size =
                stream->offset - ((uint8_t *)stream->file -
                                  (uint8_t *)stream->coverage);
        }
        info++;
    }
}

int NO_COVERAGE
coverage_snapshot(void)
{
    struct a4091_coverage *coverage;
    struct a4091_coverage *old_coverage;
    struct coverage_stream stream;

    memset(&stream, 0, sizeof (stream));
    stream.offset = sizeof (*coverage);
    coverage_serialize(&stream);
    if (stream.failed)
        return (0);

    coverage = AllocMem(stream.offset, MEMF_CLEAR | MEMF_PUBLIC);
    if (coverage == NULL)
        return (0);

    coverage->magic = A4091_COVERAGE_MAGIC;
    coverage->version = A4091_COVERAGE_VERSION;
    coverage->total_size = stream.offset;
    coverage->file_count = stream.files;

    memset(&stream, 0, sizeof (stream));
    stream.coverage = coverage;
    stream.offset = sizeof (*coverage);
    coverage_serialize(&stream);
    /* Counters can change between sizing and serialization. Never overrun
     * the allocated buffer if libgcov emits a different amount of data. */
    if (stream.failed || stream.offset != coverage->total_size ||
        stream.files != coverage->file_count) {
        FreeMem(coverage, coverage->total_size);
        return (0);
    }

    old_coverage = coverage_current;
    coverage_current = coverage;
    if (old_coverage != NULL)
        FreeMem(old_coverage, old_coverage->total_size);
    return (1);
}

void NO_COVERAGE
coverage_deinit(void)
{
    struct coverage_allocation *allocation;

    if (coverage_current != NULL) {
        FreeMem(coverage_current, coverage_current->total_size);
        coverage_current = NULL;
    }

    while (coverage_allocations != NULL) {
        allocation = coverage_allocations;
        coverage_allocations = allocation->next;
        FreeMem(allocation, allocation->size);
    }
}

struct a4091_coverage *NO_COVERAGE
coverage_data(void)
{
    return (coverage_current);
}
