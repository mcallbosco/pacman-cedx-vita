#ifndef PMCEDX_GAME_RULES_DATA_H
#define PMCEDX_GAME_RULES_DATA_H
#include <stddef.h>
/* Returns an owned CSV buffer, or NULL when the input cannot be transformed. */
char *game_rules_transform(const char *data, size_t size, size_t *output_size);
/* Only the shared PC ten-minute Score Attacks have this additional restriction. */
int game_rules_fixed_difficulty(unsigned id, unsigned map);
#endif
