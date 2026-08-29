#ifndef PMCEDX_GAME_BATCH_H
#define PMCEDX_GAME_BATCH_H

#include <stdint.h>

void game_batch_install_hooks(void);
int game_batch_direct_grid_supported(void);
void game_batch_direct_grid(void *graphics, void *buffer, unsigned vertices, unsigned stride);
int game_batch_append_sprite(void *graphics, const uint32_t vertices[4][8]);

#endif
