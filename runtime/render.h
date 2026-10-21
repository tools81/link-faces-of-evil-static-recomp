/* GPU presenter interface (render_d3d11.c). */
#pragma once
#include <stdint.h>

#define RENDER_ASPECTS 4   /* 4:3 + blurred sides, 4:3, stretch, zoom */
#ifdef HD_BUILD
/* HD edition: Cartoon HD, Cartoon HD+, then the classic filters */
#define RENDER_HD_FILTERS 2
#else
#define RENDER_HD_FILTERS 0
#endif
#define RENDER_FILTERS (RENDER_HD_FILTERS + 4)   /* ..., sharp, smooth, CRT, nearest */

typedef struct {
    int aspect;
    int filter;
} RenderSettings;

extern RenderSettings render_settings;
const char *render_aspect_name(int a);
const char *render_filter_name(int f);

#ifdef _WINDEF_
int render_init(HWND hwnd);
#endif
int render_present(const uint32_t *fb, int w, int h);
