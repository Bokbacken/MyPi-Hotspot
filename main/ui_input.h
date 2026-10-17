#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MYPI_WHEEL_ASCII_COUNT 95
#define MYPI_WHEEL_LATIN_COUNT (0x250 - 0xA0)
#define MYPI_WHEEL_COUNT       (MYPI_WHEEL_ASCII_COUNT + MYPI_WHEEL_LATIN_COUNT)

static inline uint32_t mypi_wheel_codepoint(int index)
{
    if (index < 0 || index >= MYPI_WHEEL_COUNT) return 0;
    return index < MYPI_WHEEL_ASCII_COUNT ? (uint32_t)(0x20 + index) :
           (uint32_t)(0xA0 + index - MYPI_WHEEL_ASCII_COUNT);
}

static inline int mypi_wheel_step(int index, int delta)
{
    int next = (index + delta) % MYPI_WHEEL_COUNT;
    return next < 0 ? next + MYPI_WHEEL_COUNT : next;
}

static inline int mypi_wheel_range_count(int range)
{
    return range == 0 ? MYPI_WHEEL_ASCII_COUNT : MYPI_WHEEL_LATIN_COUNT;
}

static inline uint32_t mypi_wheel_range_codepoint(int range, int index)
{
    int count = mypi_wheel_range_count(range);
    if (index < 0 || index >= count) return 0;
    return range == 0 ? (uint32_t)(0x20 + index) : (uint32_t)(0xA0 + index);
}

static inline int mypi_wheel_step_range(int range, int index, int delta)
{
    int count = mypi_wheel_range_count(range);
    int next = (index + delta) % count;
    return next < 0 ? next + count : next;
}

static inline bool mypi_utf8_append(char *text, size_t capacity, uint32_t codepoint)
{
    if (!text || !capacity || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) return false;

    uint8_t bytes[4];
    size_t count;
    if (codepoint <= 0x7F) {
        bytes[0] = (uint8_t)codepoint;
        count = 1;
    } else if (codepoint <= 0x7FF) {
        bytes[0] = (uint8_t)(0xC0 | (codepoint >> 6));
        bytes[1] = (uint8_t)(0x80 | (codepoint & 0x3F));
        count = 2;
    } else if (codepoint <= 0xFFFF) {
        bytes[0] = (uint8_t)(0xE0 | (codepoint >> 12));
        bytes[1] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[2] = (uint8_t)(0x80 | (codepoint & 0x3F));
        count = 3;
    } else {
        bytes[0] = (uint8_t)(0xF0 | (codepoint >> 18));
        bytes[1] = (uint8_t)(0x80 | ((codepoint >> 12) & 0x3F));
        bytes[2] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[3] = (uint8_t)(0x80 | (codepoint & 0x3F));
        count = 4;
    }

    size_t length = strlen(text);
    if (length + count >= capacity) return false;
    memcpy(text + length, bytes, count);
    text[length + count] = '\0';
    return true;
}

static inline bool mypi_utf8_backspace(char *text)
{
    if (!text || !text[0]) return false;
    size_t length = strlen(text);
    size_t start = length - 1;
    while (start > 0 && ((uint8_t)text[start] & 0xC0) == 0x80) start--;
    text[start] = '\0';
    return true;
}

static inline bool mypi_wifi_pass_length_valid(const char *text)
{
    if (!text) return false;
    size_t length = strlen(text);
    return length == 0 || (length >= 8 && length <= 63);
}
