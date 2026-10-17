#include "char_wheel.h"

#include <stddef.h>

#define GESTURE_LOCK_PX 12
#define MAX_GESTURE_STEPS 5

static int max_int(int a, int b)
{
    return a > b ? a : b;
}

static int clamp_int(int value, int low, int high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static char_wheel_rect_t rect(int x, int y, int w, int h)
{
    return (char_wheel_rect_t){.x = x, .y = y, .w = w, .h = h};
}

static bool contains(const char_wheel_rect_t *r, int x, int y)
{
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static int wrap(int value, int count)
{
    if (count <= 0) return 0;
    value %= count;
    return value < 0 ? value + count : value;
}

void char_wheel_layout_init(char_wheel_layout_t *layout, int width, int height, int range_count)
{
    if (!layout) return;
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (range_count < 1) range_count = 1;
    if (range_count > CHAR_WHEEL_MAX_RANGES) range_count = CHAR_WHEEL_MAX_RANGES;
    *layout = (char_wheel_layout_t){0};
    layout->width = width;
    layout->height = height;
    layout->range_count = range_count;

    int margin = max_int(2, width / 60);
    int field_gap = max_int(2, width / 60);
    int field_y = height * 10 / 100;
    int field_h = max_int(16, height * 7 / 100);
    int field_w = max_int(1, (width - 2 * margin - field_gap) / 2);
    layout->fields[0] = rect(margin, field_y, field_w, field_h);
    layout->fields[1] = rect(margin + field_w + field_gap, field_y, field_w, field_h);
    layout->preview = rect(margin, height * 18 / 100, width - 2 * margin, height * 5 / 100);

    layout->tab_y = height * 24 / 100;
    layout->tab_h = max_int(18, height * 7 / 100);
    int tab_width = width / range_count;
    for (int i = 0; i < range_count; i++) {
        int x = i * tab_width;
        int w = i == range_count - 1 ? width - x : tab_width;
        layout->tabs[i] = rect(x, layout->tab_y, w, layout->tab_h);
    }

    layout->wheel_x = width * 30 / 100;
    layout->wheel_w = max_int(1, width - 2 * layout->wheel_x);
    layout->wheel_y = height * 34 / 100;
    layout->wheel_h = max_int(1, height * 25 / 100);
    layout->item_h = max_int(12, layout->wheel_h / CHAR_WHEEL_VISIBLE_ITEMS);
    layout->vertical_step_px = max_int(12, layout->wheel_h / CHAR_WHEEL_VISIBLE_ITEMS);
    layout->horizontal_step_px = max_int(24, width / 4);

    layout->visibility = rect(width * 14 / 100, height * 68 / 100,
                              width * 72 / 100, max_int(18, height * 7 / 100));
    int action_y = height * 77 / 100;
    int action_h = max_int(26, height * 13 / 100);
    int action_w = width / 4;
    for (int i = 0; i < 4; i++) {
        int x = i * action_w;
        int w = i == 3 ? width - x : action_w;
        layout->actions[i] = rect(x, action_y, w, action_h);
    }
    layout->message = rect(margin, height * 93 / 100, width - 2 * margin, height * 6 / 100);
}

char_wheel_zone_t char_wheel_hit_test(const char_wheel_layout_t *layout, int x, int y)
{
    char_wheel_zone_t none = {.type = CHAR_WHEEL_ZONE_NONE, .index = -1};
    if (!layout) return none;
    for (int i = 0; i < 2; i++) {
        if (contains(&layout->fields[i], x, y)) return (char_wheel_zone_t){CHAR_WHEEL_ZONE_FIELD, i};
    }
    for (int i = 0; i < layout->range_count; i++) {
        if (contains(&layout->tabs[i], x, y)) return (char_wheel_zone_t){CHAR_WHEEL_ZONE_TAB, i};
    }
    if (contains(&layout->preview, x, y)) return (char_wheel_zone_t){CHAR_WHEEL_ZONE_PREVIEW, 0};
    if (x >= layout->wheel_x && x < layout->wheel_x + layout->wheel_w &&
        y >= layout->wheel_y && y < layout->wheel_y + layout->wheel_h) {
        int row = (y - layout->wheel_y) / layout->item_h;
        if (row >= CHAR_WHEEL_VISIBLE_ITEMS) row = CHAR_WHEEL_VISIBLE_ITEMS - 1;
        return (char_wheel_zone_t){CHAR_WHEEL_ZONE_ITEMS, row};
    }
    if (contains(&layout->visibility, x, y)) return (char_wheel_zone_t){CHAR_WHEEL_ZONE_VISIBILITY, 0};
    for (int i = 0; i < 4; i++) {
        if (contains(&layout->actions[i], x, y)) return (char_wheel_zone_t){CHAR_WHEEL_ZONE_ACTION, i};
    }
    return none;
}

void char_wheel_init(char_wheel_t *wheel, int range_count)
{
    if (!wheel) return;
    *wheel = (char_wheel_t){0};
    wheel->range_count = clamp_int(range_count, 1, CHAR_WHEEL_MAX_RANGES);
}

void char_wheel_set_range(char_wheel_t *wheel, int range, int character_count)
{
    if (!wheel) return;
    wheel->range = wrap(range, wheel->range_count);
    wheel->selected = wrap(wheel->selected, character_count);
    wheel->visual_shift_px = 0;
    wheel->applied_steps = 0;
}

void char_wheel_select_offset(char_wheel_t *wheel, int offset, int character_count)
{
    if (!wheel) return;
    wheel->selected = wrap(wheel->selected + offset, character_count);
    wheel->visual_shift_px = 0;
}

void char_wheel_gesture_begin(char_wheel_t *wheel, int x, int y)
{
    if (!wheel) return;
    wheel->tracking = true;
    wheel->swiped = false;
    wheel->axis = CHAR_WHEEL_AXIS_NONE;
    wheel->start_x = x;
    wheel->start_y = y;
    wheel->applied_steps = 0;
    wheel->visual_shift_px = 0;
}

bool char_wheel_gesture_move(char_wheel_t *wheel, int x, int y,
                             const char_wheel_layout_t *layout, const int *character_counts)
{
    if (!wheel || !layout || !character_counts || !wheel->tracking) return false;
    int dx = x - wheel->start_x;
    int dy = y - wheel->start_y;
    if (wheel->axis == CHAR_WHEEL_AXIS_NONE) {
        if (dx * dx + dy * dy < GESTURE_LOCK_PX * GESTURE_LOCK_PX) return false;
        wheel->axis = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
                          ? CHAR_WHEEL_AXIS_HORIZONTAL
                          : CHAR_WHEEL_AXIS_VERTICAL;
        wheel->swiped = true;
    }

    int distance = wheel->axis == CHAR_WHEEL_AXIS_HORIZONTAL ? -dx : -dy;
    int step_px = wheel->axis == CHAR_WHEEL_AXIS_HORIZONTAL
                      ? layout->horizontal_step_px
                      : layout->vertical_step_px;
    int steps = distance / step_px;
    steps = clamp_int(steps, -MAX_GESTURE_STEPS, MAX_GESTURE_STEPS);
    bool changed = false;
    int count = character_counts[wheel->range];
    if (wheel->axis == CHAR_WHEEL_AXIS_HORIZONTAL) {
        int range_steps = steps - wheel->applied_steps;
        if (range_steps) {
            wheel->range = wrap(wheel->range + range_steps, wheel->range_count);
            count = character_counts[wheel->range];
            wheel->selected = wrap(wheel->selected, count);
            wheel->applied_steps = steps;
            changed = true;
        }
        wheel->visual_shift_px = 0;
    } else {
        int character_steps = steps - wheel->applied_steps;
        if (character_steps) {
            wheel->selected = wrap(wheel->selected + character_steps, count);
            wheel->applied_steps = steps;
            changed = true;
        }
        int remainder = distance - steps * step_px;
        if (steps == MAX_GESTURE_STEPS || steps == -MAX_GESTURE_STEPS) remainder = 0;
        wheel->visual_shift_px = (float)remainder;
    }
    return changed;
}

bool char_wheel_gesture_end(char_wheel_t *wheel)
{
    if (!wheel) return false;
    bool swiped = wheel->swiped;
    wheel->tracking = false;
    wheel->axis = CHAR_WHEEL_AXIS_NONE;
    wheel->swiped = false;
    wheel->visual_shift_px = 0;
    wheel->applied_steps = 0;
    return swiped;
}
