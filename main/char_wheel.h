#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CHAR_WHEEL_MAX_RANGES 8
#define CHAR_WHEEL_VISIBLE_ITEMS 5

typedef struct {
    int x, y, w, h;
} char_wheel_rect_t;

typedef struct {
    int width, height;
    int range_count;
    int tab_y, tab_h;
    int wheel_x, wheel_y, wheel_w, wheel_h;
    int item_h;
    int vertical_step_px, horizontal_step_px;
    char_wheel_rect_t fields[2];
    char_wheel_rect_t tabs[CHAR_WHEEL_MAX_RANGES];
    char_wheel_rect_t visibility;
    char_wheel_rect_t actions[4];
    char_wheel_rect_t preview;
    char_wheel_rect_t message;
} char_wheel_layout_t;

typedef enum {
    CHAR_WHEEL_ZONE_NONE,
    CHAR_WHEEL_ZONE_FIELD,
    CHAR_WHEEL_ZONE_TAB,
    CHAR_WHEEL_ZONE_PREVIEW,
    CHAR_WHEEL_ZONE_ITEMS,
    CHAR_WHEEL_ZONE_VISIBILITY,
    CHAR_WHEEL_ZONE_ACTION,
} char_wheel_zone_type_t;

typedef struct {
    char_wheel_zone_type_t type;
    int index;
} char_wheel_zone_t;

typedef enum {
    CHAR_WHEEL_AXIS_NONE,
    CHAR_WHEEL_AXIS_HORIZONTAL,
    CHAR_WHEEL_AXIS_VERTICAL,
} char_wheel_axis_t;

typedef struct {
    int range;
    int selected;
    int range_count;
    bool tracking;
    bool swiped;
    char_wheel_axis_t axis;
    int start_x, start_y;
    int applied_steps;
    float visual_shift_px;
} char_wheel_t;

void char_wheel_layout_init(char_wheel_layout_t *layout, int width, int height, int range_count);
char_wheel_zone_t char_wheel_hit_test(const char_wheel_layout_t *layout, int x, int y);

void char_wheel_init(char_wheel_t *wheel, int range_count);
void char_wheel_set_range(char_wheel_t *wheel, int range, int character_count);
void char_wheel_select_offset(char_wheel_t *wheel, int offset, int character_count);
void char_wheel_gesture_begin(char_wheel_t *wheel, int x, int y);
bool char_wheel_gesture_move(char_wheel_t *wheel, int x, int y,
                             const char_wheel_layout_t *layout, const int *character_counts);
bool char_wheel_gesture_end(char_wheel_t *wheel);
