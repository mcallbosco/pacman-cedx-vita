#ifndef PMC_TEXT_PATCH_H
#define PMC_TEXT_PATCH_H

#include <stddef.h>

int pmcedx_patch_text_asset(const char *path, void *data, size_t len);
int pmcedx_patch_motion_blur_shader(char *source, int samples);
char *pmcedx_optimize_map_shader(const char *source);
char *pmcedx_single_texture_map_shader(const char *source);

#endif
