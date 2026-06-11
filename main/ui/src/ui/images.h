// SPIFFS + PNG mode: weather icons loaded from filesystem
#ifndef PICOPIXEL_LVGL_UI_IMAGES_H
#define PICOPIXEL_LVGL_UI_IMAGES_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef EXT_IMG_DESC_T
#define EXT_IMG_DESC_T
typedef struct _ext_img_desc_t {
    const char *name;
    const lv_img_dsc_t *img_dsc;
} ext_img_desc_t;
#endif

// Original PicoPixel image
extern const lv_img_dsc_t hpb3vuwbytl44uh;

extern const ext_img_desc_t images[];

#ifdef __cplusplus
}
#endif

#endif /*PICOPIXEL_LVGL_UI_IMAGES_H*/
