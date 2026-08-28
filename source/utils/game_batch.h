#ifndef PMCEDX_GAME_BATCH_H
#define PMCEDX_GAME_BATCH_H

void game_batch_install_hooks(void);
int game_batch_direct_grid_supported(void);
void game_batch_direct_grid(void *graphics, void *buffer, unsigned vertices, unsigned stride);

#endif
