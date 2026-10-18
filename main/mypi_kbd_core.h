#pragma once

// Pure, LVGL-free logic behind the scroll-wheel keyboard (mypi_kbd.c): character ranges,
// glyph filtering, wrap-around stepping and UTF-8 editing. Host-testable (tests/test_ui_primitives.c).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ui_input.h"

#define MYPI_KBD_TEXT_CAP 128   // Buffer size; the usable byte limit is min(max_len, cap - 1)

typedef enum {
    MYPI_KBD_RANGE_LOWER = 0,   // a-z
    MYPI_KBD_RANGE_UPPER,       // A-Z
    MYPI_KBD_RANGE_DIGIT,       // 0-9
    MYPI_KBD_RANGE_SYMBOL,      // space + ASCII punctuation
    MYPI_KBD_RANGE_LATIN,       // Swedish/Latin letters (filtered against the font)
    MYPI_KBD_RANGE_COUNT
} mypi_kbd_range_t;

// Returns true when the active font can render the code point. NULL = accept everything.
typedef bool (*mypi_kbd_glyph_fn)(uint32_t codepoint, void *ctx);

typedef struct {
    char text[MYPI_KBD_TEXT_CAP];
    size_t max_len;             // Byte limit (excluding the terminator)
    int range;                  // mypi_kbd_range_t
    int index;                  // Index into the filtered range
    mypi_kbd_glyph_fn has_glyph;
    void *glyph_ctx;
} mypi_kbd_core_t;

static const uint16_t MYPI_KBD_LATIN[] = {
    0x00E5, 0x00E4, 0x00F6, 0x00C5, 0x00C4, 0x00D6,                         // å ä ö Å Ä Ö
    0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00E0, 0x00E2, 0x00E1,                 // é è ê ë à â á
    0x00F9, 0x00FB, 0x00FC, 0x00FA, 0x00EF, 0x00EE, 0x00ED, 0x00F4, 0x00F3, // ù û ü ú ï î í ô ó
    0x00E7, 0x00E6, 0x00F8, 0x00F1, 0x00DF,                                 // ç æ ø ñ ß
    0x00C9, 0x00C8, 0x00DC, 0x00C6, 0x00D8, 0x00D1, 0x00C7,                 // É È Ü Æ Ø Ñ Ç
};
#define MYPI_KBD_LATIN_COUNT ((int)(sizeof(MYPI_KBD_LATIN) / sizeof(MYPI_KBD_LATIN[0])))

static const char MYPI_KBD_SYMBOLS[] = " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
#define MYPI_KBD_SYMBOL_COUNT ((int)sizeof(MYPI_KBD_SYMBOLS) - 1)

// Unfiltered range tables.
static inline int mypi_kbd_raw_count(int range)
{
    switch (range) {
    case MYPI_KBD_RANGE_LOWER:
    case MYPI_KBD_RANGE_UPPER: return 26;
    case MYPI_KBD_RANGE_DIGIT: return 10;
    case MYPI_KBD_RANGE_SYMBOL: return MYPI_KBD_SYMBOL_COUNT;
    case MYPI_KBD_RANGE_LATIN: return MYPI_KBD_LATIN_COUNT;
    default: return 0;
    }
}

static inline uint32_t mypi_kbd_raw_codepoint(int range, int index)
{
    if (index < 0 || index >= mypi_kbd_raw_count(range)) return 0;
    switch (range) {
    case MYPI_KBD_RANGE_LOWER: return (uint32_t)('a' + index);
    case MYPI_KBD_RANGE_UPPER: return (uint32_t)('A' + index);
    case MYPI_KBD_RANGE_DIGIT: return (uint32_t)('0' + index);
    case MYPI_KBD_RANGE_SYMBOL: return (uint8_t)MYPI_KBD_SYMBOLS[index];
    default: return MYPI_KBD_LATIN[index];
    }
}

static inline bool mypi_kbd_glyph_ok(const mypi_kbd_core_t *k, uint32_t cp)
{
    return cp && (!k->has_glyph || k->has_glyph(cp, k->glyph_ctx));
}

// Number of characters in a range that the font can render.
static inline int mypi_kbd_range_count(const mypi_kbd_core_t *k, int range)
{
    int raw = mypi_kbd_raw_count(range), count = 0;
    for (int i = 0; i < raw; i++) count += mypi_kbd_glyph_ok(k, mypi_kbd_raw_codepoint(range, i));
    return count;
}

// The index-th renderable code point of a range, or 0 if out of bounds.
static inline uint32_t mypi_kbd_range_codepoint(const mypi_kbd_core_t *k, int range, int index)
{
    int raw = mypi_kbd_raw_count(range);
    for (int i = 0; i < raw && index >= 0; i++) {
        uint32_t cp = mypi_kbd_raw_codepoint(range, i);
        if (mypi_kbd_glyph_ok(k, cp) && index-- == 0) return cp;
    }
    return 0;
}

static inline int mypi_kbd_wrap(int value, int count)
{
    if (count <= 0) return 0;
    value %= count;
    return value < 0 ? value + count : value;
}

// Selected character with a wheel offset (-2..+2 for the five visible rows).
static inline uint32_t mypi_kbd_current(const mypi_kbd_core_t *k, int offset)
{
    int count = mypi_kbd_range_count(k, k->range);
    return mypi_kbd_range_codepoint(k, k->range, mypi_kbd_wrap(k->index + offset, count));
}

static inline void mypi_kbd_step(mypi_kbd_core_t *k, int delta)
{
    k->index = mypi_kbd_wrap(k->index + delta, mypi_kbd_range_count(k, k->range));
}

// Selects a range if it has at least one renderable character.
static inline bool mypi_kbd_set_range(mypi_kbd_core_t *k, int range)
{
    if (range < 0 || range >= MYPI_KBD_RANGE_COUNT || !mypi_kbd_range_count(k, range)) return false;
    if (range != k->range) k->index = 0;
    k->range = range;
    return true;
}

// Moves to the next (dir > 0) or previous non-empty range, wrapping around.
static inline void mypi_kbd_next_range(mypi_kbd_core_t *k, int dir)
{
    for (int i = 1; i <= MYPI_KBD_RANGE_COUNT; i++) {
        if (mypi_kbd_set_range(k, mypi_kbd_wrap(k->range + (dir < 0 ? -i : i), MYPI_KBD_RANGE_COUNT))) return;
    }
}

static inline size_t mypi_kbd_utf8_len(uint8_t lead)
{
    return lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : 4;
}

static inline void mypi_kbd_core_init(mypi_kbd_core_t *k, const char *initial, size_t max_len,
                                      mypi_kbd_glyph_fn has_glyph, void *glyph_ctx)
{
    memset(k, 0, sizeof(*k));
    k->has_glyph = has_glyph;
    k->glyph_ctx = glyph_ctx;
    k->max_len = max_len && max_len < MYPI_KBD_TEXT_CAP ? max_len : MYPI_KBD_TEXT_CAP - 1;
    // Copy whole UTF-8 sequences only, so truncation never splits a character.
    size_t used = 0;
    for (const char *p = initial ? initial : ""; *p;) {
        size_t n = mypi_kbd_utf8_len((uint8_t)*p);
        size_t avail = 0;
        while (avail < n && p[avail]) avail++;      // Reject a truncated trailing sequence
        if (used + n > k->max_len || avail < n) break;
        memcpy(k->text + used, p, n);
        used += n;
        p += n;
    }
    k->range = 0;
    if (!mypi_kbd_range_count(k, k->range)) mypi_kbd_next_range(k, 1);
}

// Appends the selected character; false when it would exceed max_len.
static inline bool mypi_kbd_insert(mypi_kbd_core_t *k)
{
    uint32_t cp = mypi_kbd_current(k, 0);
    return cp && mypi_utf8_append(k->text, k->max_len + 1, cp);
}

static inline bool mypi_kbd_delete_char(mypi_kbd_core_t *k)
{
    return mypi_utf8_backspace(k->text);
}

static inline size_t mypi_kbd_char_count(const char *text)
{
    size_t n = 0;
    for (; text && *text; text++) n += ((uint8_t)*text & 0xC0) != 0x80;
    return n;
}
