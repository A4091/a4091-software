#ifndef NVRAM_FLASH_H
#define NVRAM_FLASH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// --- Type Definitions (using standard types) ---
typedef uint8_t  UBYTE;
typedef uint16_t UWORD;
typedef uint32_t ULONG;

// --- NVRAM Configuration ---
#define NVRAM_MAGIC 0x4E56524D // 'NVRM' in ASCII
#define MIN_ENTRIES 16
#define NVRAM_OFFSET 65536
#define NVRAM_SIZE 4096

// --- Error Codes ---
#define NVRAM_OK                    (0)
#define NVRAM_ERR_INVALID_ARG       (-1)
#define NVRAM_ERR_BAD_MAGIC         (-2)
#define NVRAM_ERR_VERIFY_ERASE_FAIL (-3)
#define NVRAM_ERR_NO_ENTRIES        (-4)
#define NVRAM_ERR_NO_VALID_ENTRY    (-5) // Checksum failed on all entries
#define NVRAM_ERR_FULL              (-6) // Internal error during compaction
#define NVRAM_ERR_WRITE_FAIL        (-7)

// --- Data Structures ---

// NVRAM Partition Header (8 bytes)
#pragma pack(push, 1)
struct nvram_partition_hdr {
    uint32_t magic;
    uint32_t partition_size;
};
#pragma pack(pop)

// NVRAM Entry (Length MUST be 32bit aligned)
#pragma pack(push, 1)
struct nvram_t {
    uint32_t checksum;
    union {
        uint8_t data[2];
	struct {
	    uint8_t os_flags;
	    uint8_t switch_flags;
	} settings;
    };
    uint8_t _padding[2];
};
#pragma pack(pop)

// --- NVRAM Library API ---

/**
 * @brief Formats a flash region for use as an NVRAM partition.
 *
 * @param partition_address The absolute starting address of the partition.
 * @param size The total size of the partition to create.
 * @return NVRAM_OK on success, or a negative error code.
 */
int flash_format_nvram_partition(
    ULONG partition_address,
    ULONG size
);

/**
 * @brief Reads the last valid entry from an NVRAM partition.
 *
 * @param partition_address The absolute starting address of the partition.
 * @param last_entry Pointer to buffer that will be filled with the last valid entry.
 * @return NVRAM_OK on success, or a negative error code.
 */
int flash_read_nvram(
    ULONG partition_address,
    struct nvram_t* last_entry
);

/**
 * @brief Writes a new entry to the NVRAM partition.
 *
 * @param partition_address The absolute starting address of the partition.
 * @param new_entry Pointer to the entry data to be written.
 * @return NVRAM_OK on success, or a negative error code.
 */
int flash_write_nvram(
    ULONG partition_address,
    struct nvram_t* new_entry
);

#endif // NVRAM_FLASH_H
