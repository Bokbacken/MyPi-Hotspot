#include "lvgl_view.h"

void mypi_ui_lvgl_view_init(mypi_ui_lvgl_view_t *view, lv_obj_t *parent)
{
    view->root = lv_obj_create(parent);
    lv_obj_remove_style_all(view->root);
    lv_obj_set_size(view->root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(view->root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(view->root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(view->root, LV_OBJ_FLAG_SCROLLABLE);
    for (size_t i = 0; i < MYPI_UI_STATUS_LINES; i++) {
        view->labels[i] = lv_label_create(view->root);
        lv_obj_set_width(view->labels[i], lv_pct(100));
        lv_label_set_long_mode(view->labels[i], LV_LABEL_LONG_CLIP);
        lv_label_set_text(view->labels[i], "");
    }
}

bool mypi_ui_lvgl_view_update(mypi_ui_lvgl_view_t *view, const mypi_ui_status_t *state)
{
    if (!view || !view->root || !state || state->count > MYPI_UI_STATUS_LINES ||
        (state->count && !state->lines)) return false;
    for (size_t i = 0; i < state->count; i++) {
        if (!state->lines[i].text) return false;
    }
    lv_obj_update_layout(view->root);
    int height = lv_obj_get_height(view->root);
    for (size_t i = 0; i < MYPI_UI_STATUS_LINES; i++) {
        lv_obj_t *label = view->labels[i];
        if (i >= state->count) {
            lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(label, (int)i * height / (int)state->count);
        lv_obj_set_height(label, height / (int)state->count);
        lv_obj_set_style_text_color(label, lv_color_hex(state->lines[i].color_rgb), 0);
        lv_label_set_text(label, state->lines[i].text);
    }
    return true;
}
