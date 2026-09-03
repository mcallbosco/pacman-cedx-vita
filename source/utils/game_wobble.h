#ifndef PMCEDX_GAME_WOBBLE_H
#define PMCEDX_GAME_WOBBLE_H

void game_wobble_install_hooks(void);
void game_wobble_install_late_hooks(void);
void game_wobble_prepare(void);
int game_wobble_draw(const void *vertices, unsigned count, float light);

#endif
