#include "lvgl.h"
#include "ui_helpers.h"

void _ui_screen_change( lv_obj_t *target, lv_scr_load_anim_t fademode, int spd, int delay) 
{
   lv_scr_load_anim(target, fademode, spd, delay, false);
}