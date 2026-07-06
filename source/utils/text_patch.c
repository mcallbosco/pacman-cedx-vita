#include "utils/text_patch.h"

#include <stdint.h>
#include <string.h>

static int ends_with(const char *s, const char *suffix) {
    if (!s || !suffix) return 0;
    size_t sl = strlen(s);
    size_t tl = strlen(suffix);
    return sl >= tl && strcmp(s + sl - tl, suffix) == 0;
}

static int replace_all(uint8_t *data, size_t len, const char *from, const char *to) {
    size_t flen = strlen(from);
    if (flen == 0 || flen != strlen(to) || len < flen) return 0;

    int count = 0;
    for (size_t i = 0; i <= len - flen; ++i) {
        if (memcmp(data + i, from, flen) == 0) {
            memcpy(data + i, to, flen);
            count++;
            i += flen - 1;
        }
    }
    return count;
}

static int replace_padded(uint8_t *data, size_t len, const char *from, const char *to) {
    size_t flen = strlen(from);
    size_t tlen = strlen(to);
    if (flen == 0 || tlen > flen || len < flen) return 0;

    int count = 0;
    for (size_t i = 0; i <= len - flen; ++i) {
        if (memcmp(data + i, from, flen) == 0) {
            memcpy(data + i, to, tlen);
            memset(data + i + tlen, ' ', flen - tlen);
            count++;
            i += flen - 1;
        }
    }
    return count;
}

int pmcedx_patch_text_asset(const char *path, void *data, size_t len) {
    if (!data || !ends_with(path, "texts_en.bin")) return 0;

    uint8_t *bytes = (uint8_t *)data;
    int patched = 0;
    patched += replace_padded(bytes, len, "TAP SCREEN TO START", "PRESS X TO START");
    patched += replace_all(bytes, len, "PRESS \"A\" TO START", "PRESS \"X\" TO START");
    patched += replace_padded(bytes, len, "CLICK THE TOUCHPAD TO START", "PRESS X TO START");
    patched += replace_all(bytes, len, "A - Cancel", "O - Cancel");
    patched += replace_padded(bytes, len,
                              "PRESS \"A\"/ \"D-PAD CENTER\" TO START",
                              "PRESS \"X\" TO START");
    patched += replace_padded(bytes, len,
                              "A / D-PAD CENTER  - Cancel",
                              "O - Cancel");
    patched += replace_padded(bytes, len,
                              "Press Trigger L or Trigger R to Continue",
                              "Press X to Continue");
    patched += replace_padded(bytes, len,
                              "Press <c_0x66ffffff>Trigger L</c> or <c_0x66ffffff>Trigger R</c>\n"
                              "to USE a <c_0xff6600ff>BOMB</c>.\n\n"
                              "Ghosts are sent to the nest.",
                              "Press <c_0x66ffffff>L / R</c>\n"
                              "to USE a <c_0xff6600ff>BOMB</c>.\n\n"
                              "Ghosts are sent to the nest.");
    patched += replace_padded(bytes, len,
                              "Click the <c_0x66ffffff>TOUCHPAD</c> to USE a <c_0xff6600ff>BOMB</c>.\n\n"
                              "Ghosts are sent to the nest.",
                              "Press <c_0x66ffffff>L / R</c> to USE a <c_0xff6600ff>BOMB</c>.\n\n"
                              "Ghosts are sent to the nest.");
    patched += replace_padded(bytes, len,
                              "CLICK THE TOUCHPAD TO CONTINUE",
                              "PRESS X TO CONTINUE");
    patched += replace_padded(bytes, len,
                              "Press Trigger L/ Trigger R/ D-PAD CENTER to Continue",
                              "Press X to Continue");
    patched += replace_padded(bytes, len,
                              "Press <c_0x66ffffff>Trigger L / R / D-PAD CENTER</c>\n"
                              "to USE a <c_0xff6600ff>BOMB</c>.\n\n"
                              "Ghosts are sent to the nest.",
                              "Press <c_0x66ffffff>L / R</c>\n"
                              "to USE a <c_0xff6600ff>BOMB</c>.\n\n"
                              "Ghosts are sent to the nest.");
    return patched;
}
