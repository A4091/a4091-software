// SPDX-License-Identifier: GPL-2.0-only
/* A4092 nibble word format utilities
 *
 * The A4092 uses a custom 32-bit word mapping for flash access:
 * - High nibble of byte -> bits 28..31
 * - Low nibble of byte  -> bits 12..15
 */

#ifndef NIBBLE_WORD_H
#define NIBBLE_WORD_H

#include <exec/types.h>
#include <stdint.h>

/**
 * @brief Pack a byte into the A4092 nibble word format
 * High nibble -> bits 28..31, low nibble -> bits 12..15
 */
static inline uint32_t pack_nibble_word(uint8_t b)
{
    uint32_t hi = ((uint32_t)((b >> 4) & 0x0F)) << 28;
    uint32_t lo = ((uint32_t)( b       & 0x0F)) << 12;
    return hi | lo;
}

/**
 * @brief Unpack a byte from the A4092 nibble word format
 * High nibble from bits 28..31, low nibble from bits 12..15
 */
static inline uint8_t unpack_nibble_word(uint32_t w)
{
    uint8_t hi = (uint8_t)((w >> 28) & 0x0F);
    uint8_t lo = (uint8_t)((w >> 12) & 0x0F);
    return (uint8_t)((hi << 4) | lo);
}

#endif // NIBBLE_WORD_H
