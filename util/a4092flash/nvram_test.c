#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvram_flash.h"
#include "nvram_flash.c"

// --- Flash API ---
UBYTE flash_readByte(ULONG address);
void flash_writeByte(ULONG address, UBYTE data);
void flash_erase_sector(ULONG address);

// --- Stub Flash Implementation ---
#define FLASH_SECTOR_SIZE (4 * 1024)
#define FLASH_TOTAL_SIZE  (2 * FLASH_SECTOR_SIZE)

static UBYTE flash_memory_stub[FLASH_TOTAL_SIZE];

// Stubs now convert absolute address to an offset in the memory array
void flash_erase_sector(ULONG offset)
{
    ULONG sector_start_offset = (offset / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;
    printf("STUB: Erasing sector at offset 0x%X\n", sector_start_offset);
    memset(flash_memory_stub + sector_start_offset, 0xFF, FLASH_SECTOR_SIZE);
}

void flash_writeByte(ULONG offset, UBYTE data)
{
    flash_memory_stub[offset] &= data;
}

UBYTE flash_readByte(ULONG offset)
{
    return flash_memory_stub[offset];
}

// --- Unit Test ---
void print_test_result(const char* test_name, bool pass)
{
    printf("TEST: %-44s [%s%s%s]\n", test_name, 
           pass ? "\033[32m" : "\033[31m",  // Green for PASS, Red for FAIL
           pass ? "PASS" : "FAIL",
           "\033[0m");  // Reset color
    if (!pass) exit(1);
}

int main(void)
{
    printf("NVRAM Flash Library Unit Tests\n");
    printf("--------------------------------\n");

    // The partition address is now an absolute address
    const ULONG partition_offset = FLASH_SECTOR_SIZE;
    const ULONG partition_size = FLASH_SECTOR_SIZE;
    
    // Test 1: Format and Initial State
    int res = flash_format_nvram_partition(partition_offset, partition_size);
    print_test_result("Format partition", res == NVRAM_OK);

    struct nvram_t last_entry;
    res = flash_read_nvram(partition_offset, &last_entry);
    print_test_result("Read from empty partition", res == NVRAM_ERR_NO_ENTRIES);

    // Test 2: Write and Read First Entry
    struct nvram_t entry1 = {0};
    entry1.data[0] = 0xAA;
    entry1.data[1] = 0xBB;
    res = flash_write_nvram(partition_offset, &entry1);
    print_test_result("Write first entry", res == NVRAM_OK);

    res = flash_read_nvram(partition_offset, &last_entry);
    print_test_result("Read back first entry", res == NVRAM_OK && last_entry.data[0] == 0xAA && last_entry.data[1] == 0xBB);

    // Test 3: Write a second entry
    struct nvram_t entry2 = {0};
    entry2.data[0] = 0xCC;
    entry2.data[1] = 0xDD;
    res = flash_write_nvram(partition_offset, &entry2);
    print_test_result("Write second entry", res == NVRAM_OK);
    
    res = flash_read_nvram(partition_offset, &last_entry);
    print_test_result("Read back second entry", res == NVRAM_OK && last_entry.data[0] == 0xCC && last_entry.data[1] == 0xDD);

    // Test 4: Corrupt last entry and read again
    printf("\n--- Inducing corruption ---\n");
    // Find the current last entry location in flash to corrupt it
    ULONG current_offset = sizeof(struct nvram_partition_hdr);
    ULONG last_written_offset = 0;
    while (current_offset + sizeof(struct nvram_t) <= partition_size) {
        uint32_t chk = flash_readByte(partition_offset + current_offset) |
                      (flash_readByte(partition_offset + current_offset + 1) << 8) |
                      (flash_readByte(partition_offset + current_offset + 2) << 16) |
                      (flash_readByte(partition_offset + current_offset + 3) << 24);
        if (chk == 0xFFFFFFFF) break;
        last_written_offset = current_offset;
        current_offset += sizeof(struct nvram_t);
    }
    flash_writeByte(partition_offset + last_written_offset + sizeof(uint32_t), 'X'); // Corrupt 'S'
    
    res = flash_read_nvram(partition_offset, &last_entry);
    print_test_result("Read after corruption (should find previous)", res == NVRAM_OK && last_entry.data[0] == 0xAA && last_entry.data[1] == 0xBB);
    printf("--- Corruption test passed ---\n\n");
    
    // Test 5: Fill partition to trigger compaction
    flash_format_nvram_partition(partition_offset, partition_size);
    
    int num_entries_to_fill = (partition_size - sizeof(struct nvram_partition_hdr)) / sizeof(struct nvram_t);
    printf("Filling partition with %d entries...\n", num_entries_to_fill);
    
    struct nvram_t temp_entry;
    for (int i = 0; i < num_entries_to_fill; ++i) {
        temp_entry.data[0] = (uint8_t)(i & 0xFF);
        temp_entry.data[1] = (uint8_t)((i >> 8) & 0xFF);
        res = flash_write_nvram(partition_offset, &temp_entry);
        if (res != NVRAM_OK) print_test_result("Fill loop failed", false);
    }
    print_test_result("Fill partition to capacity", true);

    struct nvram_t overflow_entry;
    overflow_entry.data[0] = 0xFF;
    overflow_entry.data[1] = 0xEE;
    printf("\nWriting one more entry to trigger compaction...\n");
    res = flash_write_nvram(partition_offset, &overflow_entry);
    print_test_result("Trigger compaction cycle", res == NVRAM_OK);

    // Verify state after compaction
    res = flash_read_nvram(partition_offset, &last_entry);
    print_test_result("Read after compaction returns newest", res == NVRAM_OK && last_entry.data[0] == 0xFF && last_entry.data[1] == 0xEE);
    
    struct nvram_t entry_in_mem;
    
    internal_flash_read(&entry_in_mem, partition_offset + sizeof(struct nvram_partition_hdr), sizeof(struct nvram_t));
    uint8_t expected_val = (num_entries_to_fill - 2) & 0xFF;
    uint8_t expected_val_hi = ((num_entries_to_fill - 2) >> 8) & 0xFF;
    print_test_result("Compaction: restored second-to-last", entry_in_mem.data[0] == expected_val && entry_in_mem.data[1] == expected_val_hi);
    
    internal_flash_read(&entry_in_mem, partition_offset + sizeof(struct nvram_partition_hdr) + sizeof(struct nvram_t), sizeof(struct nvram_t));
    expected_val = (num_entries_to_fill - 1) & 0xFF;
    expected_val_hi = ((num_entries_to_fill - 1) >> 8) & 0xFF;
    print_test_result("Compaction: restored last", entry_in_mem.data[0] == expected_val && entry_in_mem.data[1] == expected_val_hi);
    
    internal_flash_read(&entry_in_mem, partition_offset + sizeof(struct nvram_partition_hdr) + 2 * sizeof(struct nvram_t), sizeof(struct nvram_t));
    print_test_result("Compaction: wrote new entry", entry_in_mem.data[0] == 0xFF && entry_in_mem.data[1] == 0xEE);
    
    printf("\n--------------------------------\n");
    printf("All tests passed successfully! ✅\n");
    
    return 0;
}
