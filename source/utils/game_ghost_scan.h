#ifndef PMCEDX_GAME_GHOST_SCAN_H
#define PMCEDX_GAME_GHOST_SCAN_H

void game_ghost_scan_install_hooks(void);
void game_ghost_scan_reset(void);
void game_ghost_scan_call(void *task, void (*callback)(void *));
int game_ghost_scan_train(void ***list, void *ghost, void ***entry);
/* -1 requests the live sibling scan; otherwise returns its selected speed rule. */
int game_ghost_scan_spacing(void *ghost, int *near_train);

#endif
