#include "utils/text_patch.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Called on assembled, NUL-terminated GLSL before either shader-cache lookup.
 * Keep source length unchanged and recognise all supported variants so a
 * previously reduced asset can still select original quality. */
int pmcedx_patch_motion_blur_shader(char *source, int samples) {
    if (!source || !strstr(source, "u_ColorTexture") ||
        !strstr(source, "gl_FragColor = basecol/totalFact;"))
        return 0;

    char *count = strstr(source, "const float SampNum = ");
    char *weight = strstr(source, "float totalFact = ");
    if (!count || !weight)
        return 0;
    count += strlen("const float SampNum = ");
    weight += strlen("float totalFact = ");
    if (!((strncmp(count, "8.0;", 4) == 0 && strncmp(weight, "4.5;", 4) == 0) ||
          (strncmp(count, "4.0;", 4) == 0 && strncmp(weight, "2.5;", 4) == 0) ||
          (strncmp(count, "2.0;", 4) == 0 && strncmp(weight, "1.5;", 4) == 0) ||
          (strncmp(count, "1.0;", 4) == 0 && strncmp(weight, "1.0;", 4) == 0)))
        return 0;

    char selected = samples == 0 ? '1' : samples == 8 ? '8' : samples == 2 ? '2' : '4';
    int changed = count[0] != selected;
    count[0] = selected;
    /* One sample skips the offset loop and returns the original texel.
     * Otherwise weights sum to (sample count + 1) / 2. */
    weight[0] = samples == 0 ? '1' : samples == 8 ? '4' : samples == 2 ? '1' : '2';
    weight[2] = samples == 0 ? '0' : '5';
    return changed;
}

static char *replace_shader_block(const char *source, const char *from, const char *to) {
    const char *match = strstr(source, from);
    if (!match)
        return NULL;
    size_t length = strlen(source), from_length = strlen(from), to_length = strlen(to);
    if (to_length > SIZE_MAX - (length - from_length) - 1)
        return NULL;
    char *result = malloc(length - from_length + to_length + 1);
    if (!result)
        return NULL;
    size_t prefix = (size_t)(match - source);
    memcpy(result, source, prefix);
    memcpy(result + prefix, to, to_length);
    memcpy(result + prefix + to_length, match + from_length,
           length - prefix - from_length + 1);
    return result;
}

static const char map_blend_optimized[] =
    "\tvec4 base;\n"
    "\tvec4 voColor = v_oColor;\n"
    "\tif(voColor.a == 0.0) {\n"
    "\t    base = texture2D(u_diffuseMap2, v_oTexCoord2);\n"
    "\t    voColor.a = 1.0;\n"
    "\t} else {\n"
    "\t    base = texture2D(u_diffuseMap, v_oTexCoord);\n"
    "\t    if(voColor.a < 1.0) {\n"
    "\t        vec4 subColor = texture2D(u_diffuseMap2, v_oTexCoord2);\n"
    "\t        base = base*voColor.a + subColor*(1.0 - voColor.a);\n"
    "\t        voColor.a = 1.0;\n"
    "\t    }\n"
    "\t}\n";

char *pmcedx_single_texture_map_shader(const char *source) {
    if (!source)
        return NULL;
    static const char replacement[] =
        "\tvec4 base = texture2D(u_diffuseMap, v_oTexCoord);\n"
        "\tvec4 voColor = v_oColor;\n"
        "\tvoColor.a = 1.0;\n";
    return replace_shader_block(source, map_blend_optimized, replacement);
}

char *pmcedx_optimize_map_shader(const char *source) {
    if (!source || !strstr(source, "uniform sampler2D u_diffuseMap2;"))
        return NULL;

    static const char blend[] =
        "\tvec4 base = texture2D( u_diffuseMap, v_oTexCoord );\n"
        "\tvec4 voColor = v_oColor;\n"
        "\tif(voColor.a<1.0)\n"
        "\t{\n"
        "\t    vec4 subColor = texture2D( u_diffuseMap2, v_oTexCoord2 );\n"
        "\t    base = base*(voColor.a) + subColor*(1.0 - voColor.a);\n"
        "\t    voColor.a = 1.0;\n"
        "\t}\n";
    char *result = replace_shader_block(source, blend, map_blend_optimized);
    if (!result)
        return NULL;

    static const char tint[] = "\n    if(u_spDeltaHSV.w < 1.5)\n";
    static const char tint_optimized[] =
        "\n    if(u_spDeltaHSV.w < 1.5 && u_spDeltaHSV.x == 0.0)\n"
        "    {\n"
        "        oColor = base*voColor;\n"
        "        oColor.xyz = min(oColor.xyz*u_spDeltaHSV.z, vec3(1.0));\n"
        "    }\n"
        "    else if(u_spDeltaHSV.w < 1.5)\n";
    char *with_tint = replace_shader_block(result, tint, tint_optimized);
    if (with_tint) {
        free(result);
        result = with_tint;
    }
    return result;
}

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
