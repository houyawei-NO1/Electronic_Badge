// SPIFFS + PNG mode: weather icons loaded from filesystem
// Only PicoPixel image remains as C array
#include "images.h"

// Original PicoPixel image
#include "images/hpb3vuwbytl44uh.c"

const ext_img_desc_t images[] = {
    { "hpb3vuwbytl44uh", &hpb3vuwbytl44uh },
    { NULL, NULL }  // Sentinel
};
