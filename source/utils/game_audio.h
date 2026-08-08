#ifndef PMCEDX_GAME_AUDIO_H
#define PMCEDX_GAME_AUDIO_H

void game_audio_install_hooks(void);

#ifdef ENABLE_AUDIO_LOGS
void game_audio_log_play(void *sound, void *group, int result, void *channel);
void game_audio_log_release(void *sound);
#endif

#endif
