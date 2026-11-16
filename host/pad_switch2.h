/* (hookshot) The Nintendo Switch 2 Pro Controller over USB, read by the host itself (host/pad_switch2.c). host/hos_sdl.c lists
   its pads among SDL's and hands the hos_sdl_pad_* calls for one here, so host/pad_sdl.c treats it as any other pad. On
   other systems these find nothing (SDL built with libusb has its own driver for it there). */
#pragma once
#include <stdint.h>

/* the pads found (open or not) appended as ids and open codes, as hos_sdl_pads_list gives them; how many */
int hos_switch2_list(int32_t *ids, int *opens, int max);
int hos_switch2_code(int what);    /* 1 when an open code is one of these */
void *hos_switch2_open(int what);  /* NULL: not now (said in the log) */
int hos_switch2_is(const void *pad);
void hos_switch2_close(void *pad);
int hos_switch2_connected(void *pad);
void hos_switch2_update(void);     /* every poll: the reports that came, the rumble due */
int hos_switch2_button(void *pad, int button);   /* SDL3's gamepad button numbers (by position) */
int16_t hos_switch2_axis(void *pad, int axis);   /* SDL's gamepad axes: the sticks (up negative), then the triggers */
const char *hos_switch2_name(void *pad);
int32_t hos_switch2_id(void *pad);
void hos_switch2_rumble(void *pad, uint16_t low, uint16_t high);
int hos_switch2_sensor(void *pad, int type, float *data, int n);   /* type: HOS_SDL_SENSOR_*; 1 when read */
