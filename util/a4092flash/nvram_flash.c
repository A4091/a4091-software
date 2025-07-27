#include "flash.h"
#include "nvram_flash.h"
#include <string.h> // For memcpy

// --- Internal Helper Functions ---

static uint32_t calculate_checksum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return sum;
}

static void internal_flash_write(ULONG base_addr, const void* src, size_t size)
{
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < size; ++i) {
        flash_writeByte(base_addr + i, s[i]);
    }
}

static void internal_flash_read(void* dest, ULONG base_addr, size_t size)
{
    uint8_t* d = (uint8_t*)dest;
    for (size_t i = 0; i < size; ++i) {
        d[i] = flash_readByte(base_addr + i);
    }
}

static bool validate_entry(const struct nvram_t *entry)
{
    return calculate_checksum(entry->data, sizeof(entry->data)) == entry->checksum;
}

static void find_entries(ULONG partition_address, const struct nvram_partition_hdr *hdr, 
                        ULONG *last_offset, ULONG *second_last_offset, ULONG *free_slot_offset)
{
    const ULONG data_area_start = sizeof(struct nvram_partition_hdr);
    ULONG current_offset = data_area_start;
    *last_offset = 0;
    *second_last_offset = 0;
    if (free_slot_offset) *free_slot_offset = 0;

    while (current_offset + sizeof(struct nvram_t) <= hdr->partition_size) {
        uint32_t chk;
        internal_flash_read(&chk, partition_address + current_offset, sizeof(uint32_t));
        if (chk == 0xFFFFFFFF) {
            if (free_slot_offset && *free_slot_offset == 0) {
                *free_slot_offset = current_offset;
            }
            break;
        }
        *second_last_offset = *last_offset;
        *last_offset = current_offset;
        current_offset += sizeof(struct nvram_t);
    }
}

// --- API Implementation ---

int flash_format_nvram_partition(ULONG partition_address, ULONG size)
{
    if (size < sizeof(struct nvram_partition_hdr)) {
        return NVRAM_ERR_INVALID_ARG;
    }

    flash_erase_sector(partition_address);

    for (ULONG i = 0; i < size; ++i) {
        if (flash_readByte(partition_address + i) != 0xFF) {
            return NVRAM_ERR_VERIFY_ERASE_FAIL;
        }
    }

    struct nvram_partition_hdr hdr = { .magic = NVRAM_MAGIC, .partition_size = size };
    internal_flash_write(partition_address, &hdr, sizeof(hdr));

    struct nvram_partition_hdr read_hdr;
    internal_flash_read(&read_hdr, partition_address, sizeof(read_hdr));
    if (read_hdr.magic != hdr.magic || read_hdr.partition_size != hdr.partition_size) {
        return NVRAM_ERR_WRITE_FAIL;
    }

    return NVRAM_OK;
}

int flash_read_nvram(ULONG partition_address, struct nvram_t* last_entry)
{
    if (last_entry == NULL) return NVRAM_ERR_INVALID_ARG;

    struct nvram_partition_hdr hdr;
    internal_flash_read(&hdr, partition_address, sizeof(hdr));

    if (hdr.magic != NVRAM_MAGIC) return NVRAM_ERR_BAD_MAGIC;
    if (hdr.partition_size < sizeof(struct nvram_partition_hdr) + MIN_ENTRIES * sizeof(struct nvram_t)) {
        return NVRAM_ERR_INVALID_ARG;
    }

    ULONG last_offset, second_last_offset;
    find_entries(partition_address, &hdr, &last_offset, &second_last_offset, NULL);

    if (last_offset == 0) return NVRAM_ERR_NO_ENTRIES;

    const ULONG data_area_start = sizeof(struct nvram_partition_hdr);
    ULONG current_offset = last_offset;
    
    while (current_offset >= data_area_start) {
        struct nvram_t entry;
        internal_flash_read(&entry, partition_address + current_offset, sizeof(entry));

        if (validate_entry(&entry)) {
            *last_entry = entry;
            return NVRAM_OK;
        }

        if (current_offset < data_area_start + sizeof(struct nvram_t)) break;
        current_offset -= sizeof(struct nvram_t);
    }

    return NVRAM_ERR_NO_VALID_ENTRY;
}

int flash_write_nvram(ULONG partition_address, struct nvram_t* new_entry)
{
    if (new_entry == NULL) return NVRAM_ERR_INVALID_ARG;

    struct nvram_partition_hdr hdr;
    internal_flash_read(&hdr, partition_address, sizeof(hdr));

    if (hdr.magic != NVRAM_MAGIC) return NVRAM_ERR_BAD_MAGIC;
    if (hdr.partition_size < sizeof(struct nvram_partition_hdr) + MIN_ENTRIES * sizeof(struct nvram_t)) {
        return NVRAM_ERR_INVALID_ARG;
    }

    const ULONG data_area_start = sizeof(struct nvram_partition_hdr);
    ULONG last_offset, second_last_offset, free_slot_offset;
    find_entries(partition_address, &hdr, &last_offset, &second_last_offset, &free_slot_offset);

    if (free_slot_offset == 0) {

        struct nvram_t last = {0}, second_last = {0};
        bool last_found = false, second_last_found = false;

        if (last_offset != 0) {
            internal_flash_read(&last, partition_address + last_offset, sizeof(last));
            if (validate_entry(&last)) last_found = true;
        }

        if (second_last_offset != 0) {
            internal_flash_read(&second_last, partition_address + second_last_offset, sizeof(second_last));
            if (validate_entry(&second_last)) second_last_found = true;
        }
        
        int format_res = flash_format_nvram_partition(partition_address, hdr.partition_size);
        if (format_res != NVRAM_OK) return format_res;

        ULONG write_offset = data_area_start;
        if (second_last_found) {
            internal_flash_write(partition_address + write_offset, &second_last, sizeof(struct nvram_t));
            write_offset += sizeof(struct nvram_t);
        }
        if (last_found) {
            internal_flash_write(partition_address + write_offset, &last, sizeof(struct nvram_t));
            write_offset += sizeof(struct nvram_t);
        }
        free_slot_offset = write_offset;
    }
    
    if (free_slot_offset == 0) return NVRAM_ERR_FULL;

    struct nvram_t entry_to_write;
    memcpy(entry_to_write.data, new_entry->data, sizeof(entry_to_write.data));
    memset(entry_to_write._padding, 0, sizeof(entry_to_write._padding));
    entry_to_write.checksum = calculate_checksum(entry_to_write.data, sizeof(entry_to_write.data));
    
    internal_flash_write(partition_address + free_slot_offset, &entry_to_write, sizeof(entry_to_write));

    return NVRAM_OK;
}
