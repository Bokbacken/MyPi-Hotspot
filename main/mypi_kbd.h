#pragma once

// Reusable on-screen text input module (LVGL). The caller decides placement and size; every
// part of the layout scales from w/h, and the character set is filtered against the font.

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

// Called with confirmed=true on OK and confirmed=false on BACK (cancel). The text pointer is
// only valid during the call. After BACK the keyboard deletes itself; after OK the owner
// decides (call mypi_kbd_delete() to close, or keep it open e.g. after mypi_kbd_set_hint()).
typedef void (*mypi_kbd_cb_t)(const char *text, bool confirmed, void *user);

typedef enum {
    MYPI_KBD_STYLE_WHEEL = 0,   // Companion-style scroll wheel, for small touch screens
    // MYPI_KBD_STYLE_FULLKEY   // Reserved: full on-screen keyboard for large screens
} mypi_kbd_style_t;

// Creates the keyboard as a child of parent at (x, y) with size w x h. Unknown styles fall back
// to MYPI_KBD_STYLE_WHEEL. max_len is in UTF-8 bytes (clamped to 127); mask shows '*' per
// character (passwords). Returns NULL if LVGL is out of memory.
lv_obj_t *mypi_kbd_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                          mypi_kbd_style_t style, const char *label, const char *initial_text,
                          size_t max_len, bool mask, mypi_kbd_cb_t cb, void *user);

// Safe to call from inside the callback/LVGL events and more than once; the text buffer is wiped.
void mypi_kbd_delete(lv_obj_t *kbd);

// Shows a temporary message in the hint row (cleared by the next interaction).
void mypi_kbd_set_hint(lv_obj_t *kbd, const char *message);

// Current text (valid until the next interaction), or NULL if kbd is not an open keyboard.
const char *mypi_kbd_get_text(lv_obj_t *kbd);
