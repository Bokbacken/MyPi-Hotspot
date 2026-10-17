#pragma once

#include <stddef.h>
#include <stdint.h>

#define MYPI_UART_BITS_PER_BYTE 10

static inline void mypi_uart_frame(uint8_t byte, uint8_t bits[MYPI_UART_BITS_PER_BYTE])
{
    bits[0] = 0;
    for (unsigned i = 0; i < 8; i++) bits[i + 1] = (byte >> i) & 1u;
    bits[9] = 1;
}

static inline size_t mypi_uart_frame_text(const char *text, uint8_t *bits, size_t capacity)
{
    if (!text || !bits) return 0;
    size_t count = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (capacity - count < MYPI_UART_BITS_PER_BYTE) break;
        mypi_uart_frame(*p, bits + count);
        count += MYPI_UART_BITS_PER_BYTE;
    }
    return count;
}

static inline int mypi_uart_x(int x, size_t bit_index, int bit_width)
{
    return x + (int)bit_index * bit_width;
}

static inline int mypi_uart_y(int high_y, int low_y, uint8_t level)
{
    return level ? high_y : low_y;
}
