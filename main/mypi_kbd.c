#include "mypi_kbd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mypi_kbd_core.h"

LV_FONT_DECLARE(mypi_font_14);

#define KBD_MAGIC      0x4B424431u     // "KBD1"
#define KBD_ROWS       5
#define KBD_BUTTONS    4
// Finger travel per character step, in percent of a small wheel row (~29 px on 240x320).
// Distance-based (not time-based), so it also acts as the wheel's debounce/inertia.
/* v0.8.8: slowed scroll debounce per user feedback */
#define MYPI_KBD_SCROLL_STEP_PCT 100   // Was 50 (half a row) in 0.8.7
#define COL_BG         0x000000
#define COL_TEXT       0xFFFFFF
#define COL_MUTED      0x8C8C8C
#define COL_DIM        0x5A5A5A
#define COL_ACCENT     0xFF8C1A
#define COL_ACCENT_BG  0x3A2000
#define COL_CYAN       0x00C8FF
#define COL_HINT_MSG   0xFFB000

// Row height ratios of the 5-slot wheel (-2, -1, current, +1, +2), as on MyPi-Companion.
static const uint8_t ROW_RATIO[KBD_ROWS] = {20, 20, 32, 20, 20};
static const char *const BUTTON_TEXT[KBD_BUTTONS] = {"BACK", "DEL", "NEXT", "OK"};
static const uint32_t BUTTON_COLOR[KBD_BUTTONS] = {COL_MUTED, 0xFF4040, COL_CYAN, COL_ACCENT};

typedef struct {
    uint32_t magic;
    mypi_kbd_core_t core;
    const lv_font_t *font;
    mypi_kbd_cb_t cb;
    void *user;
    bool mask;
    bool closing;
    lv_obj_t *root;
    lv_obj_t *field_text;
    lv_obj_t *tabs[MYPI_KBD_RANGE_COUNT];
    lv_obj_t *hint;
    lv_obj_t *wheel;
    lv_obj_t *rows[KBD_ROWS];
    lv_obj_t *row_labels[KBD_ROWS];
    int32_t row_y[KBD_ROWS + 1];    // Cumulative row offsets inside the wheel
    int32_t field_inner_w;
    bool hint_message;
    // Wheel gesture state
    int32_t tot_x, tot_y, acc_y;
    uint8_t mode;                   // 0 undecided, 1 vertical (chars), 2 horizontal (ranges), 3 done
} kbd_t;

static bool font_has_glyph(uint32_t cp, void *ctx)
{
    lv_font_glyph_dsc_t dsc;
    return lv_font_get_glyph_dsc((const lv_font_t *)ctx, &dsc, cp, 0) && dsc.adv_w > 0;
}

static kbd_t *kbd_get(lv_obj_t *obj)
{
    kbd_t *k = obj ? (kbd_t *)lv_obj_get_user_data(obj) : NULL;
    return k && k->magic == KBD_MAGIC ? k : NULL;
}

static void encode_char(uint32_t cp, char *out, size_t cap, bool current)
{
    out[0] = '\0';
    if (cp == ' ') {
        snprintf(out, cap, "%s", current ? "SPACE" : "spc");
    } else if (cp) {
        mypi_utf8_append(out, cap, cp);
    }
}

static lv_obj_t *plain_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void refresh_field(kbd_t *k)
{
    char shown[MYPI_KBD_TEXT_CAP + 2];
    if (k->mask) {
        size_t n = mypi_kbd_char_count(k->core.text);
        if (n > MYPI_KBD_TEXT_CAP - 1) n = MYPI_KBD_TEXT_CAP - 1;
        memset(shown, '*', n);
        shown[n] = '\0';
    } else {
        snprintf(shown, sizeof(shown), "%s", k->core.text);
    }
    size_t used = strlen(shown);
    shown[used] = '_';                      // Cursor (shown has room for text + cursor + NUL)
    shown[used + 1] = '\0';
    // Keep the end (cursor) visible: drop leading characters until the text fits.
    const char *p = shown;
    lv_point_t size;
    for (;;) {
        lv_text_get_size(&size, p, k->font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x <= k->field_inner_w || p[1] == '\0') break;
        p += mypi_kbd_utf8_len((uint8_t)*p);
    }
    lv_label_set_text(k->field_text, p);
    memset(shown, 0, sizeof(shown));
}

static void refresh(kbd_t *k)
{
    refresh_field(k);
    for (int r = 0; r < MYPI_KBD_RANGE_COUNT; r++) {
        if (!k->tabs[r]) continue;
        bool active = r == k->core.range;
        lv_obj_set_style_bg_opa(k->tabs[r], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(k->tabs[r], lv_color_hex(active ? COL_ACCENT : COL_DIM), 0);
        lv_obj_set_style_text_color(k->tabs[r], lv_color_hex(active ? COL_TEXT : COL_MUTED), 0);
    }
    if (!k->hint_message) {
        lv_obj_set_style_text_color(k->hint, lv_color_hex(COL_MUTED), 0);
        lv_label_set_text(k->hint, "< range >   ^ char v");
    }
    for (int i = 0; i < KBD_ROWS; i++) {
        char buf[8];
        encode_char(mypi_kbd_current(&k->core, i - 2), buf, sizeof(buf), i == 2);
        lv_label_set_text(k->row_labels[i], buf);
    }
}

static void clear_hint(kbd_t *k)
{
    k->hint_message = false;
}

static void tab_event(lv_event_t *e)
{
    kbd_t *k = (kbd_t *)lv_event_get_user_data(e);
    lv_obj_t *tab = lv_event_get_current_target(e);
    if (k->closing) return;
    for (int r = 0; r < MYPI_KBD_RANGE_COUNT; r++) {
        if (k->tabs[r] == tab) mypi_kbd_set_range(&k->core, r);
    }
    clear_hint(k);
    refresh(k);
}

static void wheel_event(lv_event_t *e)
{
    kbd_t *k = (kbd_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (k->closing || !indev) return;
    int32_t small_h = k->row_y[1] - k->row_y[0];
    int32_t step_px = LV_MAX(8, small_h * MYPI_KBD_SCROLL_STEP_PCT / 100);
    int32_t slop = LV_MAX(6, small_h / 3);
    int32_t swipe_px = LV_MAX(24, lv_obj_get_width(k->wheel) / 5);
    bool changed = false;

    if (code == LV_EVENT_PRESSED) {
        k->tot_x = k->tot_y = k->acc_y = 0;
        k->mode = 0;
    } else if (code == LV_EVENT_PRESSING) {
        lv_point_t v;
        lv_indev_get_vect(indev, &v);
        k->tot_x += v.x;
        k->tot_y += v.y;
        if (k->mode == 0 && (LV_ABS(k->tot_x) > slop || LV_ABS(k->tot_y) > slop)) {
            k->mode = LV_ABS(k->tot_x) > LV_ABS(k->tot_y) ? 2 : 1;
            k->acc_y = k->tot_y;
        } else if (k->mode == 1) {
            k->acc_y += v.y;
        }
        if (k->mode == 1) {
            // Finger down pulls the previous characters into the center, like a physical wheel.
            while (k->acc_y >= step_px) { mypi_kbd_step(&k->core, -1); k->acc_y -= step_px; changed = true; }
            while (k->acc_y <= -step_px) { mypi_kbd_step(&k->core, 1); k->acc_y += step_px; changed = true; }
        } else if (k->mode == 2 && LV_ABS(k->tot_x) >= swipe_px) {
            mypi_kbd_next_range(&k->core, k->tot_x < 0 ? 1 : -1);   // Swipe left = next range
            k->mode = 3;                                            // One range per swipe
            changed = true;
        }
    } else if (code == LV_EVENT_RELEASED) {
        if (k->mode == 0) {
            // Tap: the touched row moves to the center (-2..+2).
            lv_point_t p;
            lv_area_t a;
            lv_indev_get_point(indev, &p);
            lv_obj_get_coords(k->wheel, &a);
            int32_t y = p.y - a.y1;
            for (int i = 0; i < KBD_ROWS; i++) {
                if (y >= k->row_y[i] && y < k->row_y[i + 1] && i != 2) {
                    mypi_kbd_step(&k->core, i - 2);
                    changed = true;
                }
            }
        }
        k->mode = 0;
    }
    if (changed) {
        clear_hint(k);
        refresh(k);
    }
}

// Calls the owner with a private copy of the text, so the owner may close the keyboard
// (which wipes core.text) inside the callback and still use the text it was given.
static void notify(kbd_t *k, bool confirmed)
{
    char copy[MYPI_KBD_TEXT_CAP];
    if (!k->cb) return;
    snprintf(copy, sizeof(copy), "%s", k->core.text);
    k->cb(copy, confirmed, k->user);
    memset(copy, 0, sizeof(copy));
}

static void button_event(lv_event_t *e)
{
    kbd_t *k = (kbd_t *)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target(e);
    if (k->closing) return;
    int id = (int)(intptr_t)lv_obj_get_user_data(btn);
    clear_hint(k);
    switch (id) {
    case 0:     // BACK: cancel, then close
        notify(k, false);
        mypi_kbd_delete(k->root);
        return;
    case 1:
        mypi_kbd_delete_char(&k->core);
        break;
    case 2:
        if (!mypi_kbd_insert(&k->core)) mypi_kbd_set_hint(k->root, "Max length reached");
        break;
    default:    // OK: the owner validates and closes
        notify(k, true);
        return;
    }
    refresh(k);
}

static void root_delete_event(lv_event_t *e)
{
    kbd_t *k = (kbd_t *)lv_event_get_user_data(e);
    memset(k, 0, sizeof(*k));   // Wipes the (possibly secret) text
    lv_free(k);
}

lv_obj_t *mypi_kbd_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                          mypi_kbd_style_t style, const char *label, const char *initial_text,
                          size_t max_len, bool mask, mypi_kbd_cb_t cb, void *user)
{
    // Only the wheel style exists so far; FULLKEY (large screens) and unknown values fall back.
    (void)style;
    if (!parent || w < 60 || h < 80) return NULL;
    kbd_t *k = (kbd_t *)lv_malloc(sizeof(kbd_t));
    if (!k) return NULL;
    memset(k, 0, sizeof(*k));
    k->magic = KBD_MAGIC;
    k->font = &mypi_font_14;
    k->cb = cb;
    k->user = user;
    k->mask = mask;
    mypi_kbd_core_init(&k->core, initial_text, max_len, font_has_glyph, (void *)k->font);

    // Proportional layout: field 14%, tabs 10%, hint 7%, buttons 18%, wheel the rest (~51%).
    int32_t pad = LV_MAX(2, w / 60);
    int32_t gap = LV_MAX(2, w / 80);
    int32_t field_h = h * 14 / 100, tabs_h = h * 10 / 100, hint_h = h * 7 / 100, btn_h = h * 18 / 100;
    int32_t wheel_h = h - field_h - tabs_h - hint_h - btn_h;
    int32_t inner_w = w - 2 * pad;
    int32_t line_h = lv_font_get_line_height(k->font);

    lv_obj_t *root = lv_obj_create(parent);
    k->root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, x, y);
    lv_obj_set_size(root, w, h);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(root, k->font, 0);
    lv_obj_set_style_text_color(root, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_user_data(root, k);
    lv_obj_add_event_cb(root, root_delete_event, LV_EVENT_DELETE, k);

    // Field: label (muted) above the entered text with cursor.
    lv_obj_t *field = plain_box(root, pad, pad, inner_w, field_h - pad);
    lv_obj_set_style_border_width(field, 1, 0);
    lv_obj_set_style_border_color(field, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_radius(field, LV_MAX(2, h / 80), 0);
    int32_t field_inner_h = field_h - pad - 2;
    k->field_inner_w = inner_w - 2 * pad - 2;
    if (label && label[0] && field_inner_h >= 2 * line_h) {
        lv_obj_t *title = lv_label_create(field);
        lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(title, k->field_inner_w);
        lv_obj_set_style_text_color(title, lv_color_hex(COL_MUTED), 0);
        lv_label_set_text(title, label);
        lv_obj_set_pos(title, pad, 1);
    }
    k->field_text = lv_label_create(field);
    lv_label_set_long_mode(k->field_text, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(k->field_text, k->field_inner_w);
    lv_obj_set_pos(k->field_text, pad, LV_MAX(1, field_inner_h - line_h));

    // Range tabs, only for ranges the font can render.
    static const char *const TAB_TEXT[MYPI_KBD_RANGE_COUNT] = {"az", "AZ", "09", "#+", "\xC3\xA5\xC3\xA4"};
    int visible = 0;
    for (int r = 0; r < MYPI_KBD_RANGE_COUNT; r++) visible += mypi_kbd_range_count(&k->core, r) > 0;
    int32_t tab_y = field_h + gap / 2, tab_h = tabs_h - gap;
    int32_t tab_w = visible ? (inner_w - (visible - 1) * gap) / visible : inner_w;
    for (int r = 0, slot = 0; r < MYPI_KBD_RANGE_COUNT; r++) {
        if (!mypi_kbd_range_count(&k->core, r)) continue;
        lv_obj_t *tab = plain_box(root, pad + slot++ * (tab_w + gap), tab_y, tab_w, tab_h);
        lv_obj_add_flag(tab, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(tab, lv_color_hex(COL_ACCENT_BG), 0);
        lv_obj_set_style_border_width(tab, 1, 0);
        lv_obj_set_style_radius(tab, 3, 0);
        lv_obj_set_ext_click_area(tab, gap / 2);
        lv_obj_t *text = lv_label_create(tab);
        bool latin_ok = font_has_glyph(0xE5, (void *)k->font) && font_has_glyph(0xE4, (void *)k->font);
        lv_label_set_text(text, r == MYPI_KBD_RANGE_LATIN && !latin_ok ? "Lt" : TAB_TEXT[r]);
        lv_obj_center(text);
        lv_obj_add_event_cb(tab, tab_event, LV_EVENT_CLICKED, k);
        k->tabs[r] = tab;
    }

    // Hint row.
    int32_t hint_y = field_h + tabs_h;
    k->hint = lv_label_create(root);
    lv_label_set_long_mode(k->hint, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(k->hint, inner_w);
    lv_obj_set_style_text_align(k->hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(k->hint, pad, hint_y + LV_MAX(0, (hint_h - line_h) / 2));

    // Wheel: five rows, the current one highlighted; it receives all drag/tap input.
    int32_t wheel_y = hint_y + hint_h;
    k->wheel = plain_box(root, pad, wheel_y, inner_w, wheel_h);
    lv_obj_add_flag(k->wheel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(k->wheel, LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_GESTURE_BUBBLE);
    int32_t ratio_sum = 0;
    for (int i = 0; i < KBD_ROWS; i++) ratio_sum += ROW_RATIO[i];
    int32_t small_h = wheel_h * ROW_RATIO[0] / ratio_sum;
    for (int i = 0; i < KBD_ROWS; i++) {
        int32_t row_h = i == 2 ? wheel_h - 4 * small_h : small_h;
        k->row_y[i + 1] = k->row_y[i] + row_h;
        int32_t inset = i == 2 ? 0 : inner_w / 8;
        lv_obj_t *row = plain_box(k->wheel, inset, k->row_y[i], inner_w - 2 * inset, row_h - 1);
        k->rows[i] = row;
        k->row_labels[i] = lv_label_create(row);
        lv_obj_center(k->row_labels[i]);
        if (i == 2) {
            // No larger font is built in, so the current row is emphasized with an accent box.
            lv_obj_set_style_bg_color(row, lv_color_hex(COL_ACCENT_BG), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(row, LV_MAX(2, h / 120), 0);
            lv_obj_set_style_border_color(row, lv_color_hex(COL_ACCENT), 0);
            lv_obj_set_style_radius(row, LV_MAX(3, h / 60), 0);
            lv_obj_set_style_text_color(row, lv_color_hex(COL_TEXT), 0);
        } else {
            lv_obj_set_style_text_color(row, lv_color_hex(i == 1 || i == 3 ? COL_MUTED : COL_DIM), 0);
        }
    }
    lv_obj_t *up = lv_label_create(k->wheel);
    lv_label_set_text(up, "^");
    lv_obj_set_style_text_color(up, lv_color_hex(COL_ACCENT), 0);
    lv_obj_align(up, LV_ALIGN_TOP_RIGHT, -pad, 0);
    lv_obj_t *down = lv_label_create(k->wheel);
    lv_label_set_text(down, "v");
    lv_obj_set_style_text_color(down, lv_color_hex(COL_ACCENT), 0);
    lv_obj_align(down, LV_ALIGN_BOTTOM_RIGHT, -pad, 0);
    lv_obj_add_event_cb(k->wheel, wheel_event, LV_EVENT_PRESSED, k);
    lv_obj_add_event_cb(k->wheel, wheel_event, LV_EVENT_PRESSING, k);
    lv_obj_add_event_cb(k->wheel, wheel_event, LV_EVENT_RELEASED, k);

    // Bottom row: BACK / DEL / NEXT / OK.
    int32_t btn_y = h - btn_h + gap, bh = btn_h - gap - pad;
    int32_t bw = (inner_w - (KBD_BUTTONS - 1) * gap) / KBD_BUTTONS;
    for (int i = 0; i < KBD_BUTTONS; i++) {
        lv_obj_t *btn = lv_button_create(root);
        lv_obj_remove_style_all(btn);
        lv_obj_set_pos(btn, pad + i * (bw + gap), btn_y);
        lv_obj_set_size(btn, bw, bh);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(BUTTON_COLOR[i]), 0);
        lv_obj_set_style_radius(btn, LV_MAX(2, h / 80), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(BUTTON_COLOR[i]), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(btn, lv_color_hex(BUTTON_COLOR[i]), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(COL_BG), LV_STATE_PRESSED);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_t *text = lv_label_create(btn);
        lv_label_set_text(text, BUTTON_TEXT[i]);
        lv_obj_center(text);
        lv_obj_add_event_cb(btn, button_event, LV_EVENT_CLICKED, k);
    }

    refresh(k);
    return root;
}

void mypi_kbd_delete(lv_obj_t *kbd)
{
    kbd_t *k = kbd_get(kbd);
    if (!k || k->closing) return;
    k->closing = true;
    memset(k->core.text, 0, sizeof(k->core.text));
    lv_obj_delete_async(kbd);
}

void mypi_kbd_set_hint(lv_obj_t *kbd, const char *message)
{
    kbd_t *k = kbd_get(kbd);
    if (!k || k->closing) return;
    k->hint_message = message && message[0];
    if (k->hint_message) {
        lv_obj_set_style_text_color(k->hint, lv_color_hex(COL_HINT_MSG), 0);
        lv_label_set_text(k->hint, message);
    }
    refresh(k);
}

const char *mypi_kbd_get_text(lv_obj_t *kbd)
{
    kbd_t *k = kbd_get(kbd);
    return k && !k->closing ? k->core.text : NULL;
}
