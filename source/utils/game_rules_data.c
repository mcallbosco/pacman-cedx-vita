#include "game_rules_data.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pc_course_rule {
    unsigned id, map;
    unsigned values[28]; /* StageData columns 11..38, including empty targets. */
};
#include "game_rules_table.h"

struct field { const char *text; size_t size; };

static unsigned number(struct field f) {
    unsigned n = 0;
    if (!f.size || f.size > 5)
        return UINT32_MAX;
    for (size_t i = 0; i < f.size; ++i) {
        if (f.text[i] < '0' || f.text[i] > '9')
            return UINT32_MAX;
        n = n * 10 + (unsigned)(f.text[i] - '0');
    }
    return n;
}

static const struct pc_course_rule *find_rule(unsigned id, unsigned map) {
    size_t lo = 0, hi = sizeof(pc_courses) / sizeof(pc_courses[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (pc_courses[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    if (lo == sizeof(pc_courses) / sizeof(pc_courses[0]) ||
        pc_courses[lo].id != id || pc_courses[lo].map != map)
        return NULL;
    return &pc_courses[lo];
}

int game_rules_fixed_difficulty(unsigned id, unsigned map) {
    return id >= 11 && id <= 16 && find_rule(id, map) != NULL;
}

char *game_rules_transform(const char *data, size_t size, size_t *output_size) {
    if (!data || !output_size || !size || size > 1024 * 1024)
        return NULL;
    /* Every recognized native row has at least 42 fields. Four times its input
     * length covers normal expansion; bounds below also reject malformed data. */
    size_t capacity = size * 4 + 1, written = 0;
    char *output = malloc(capacity);
    if (!output) return NULL;
    for (size_t start = 0; start < size;) {
        size_t end = start;
        while (end < size && data[end] != '\r' && data[end] != '\n') ++end;
        struct field fields[52];
        size_t count = 0, begin = start;
        for (size_t at = start; at <= end; ++at) {
            if (at == end || data[at] == ',') {
                if (count == 52) goto invalid;
                fields[count++] = (struct field){data + begin, at - begin};
                begin = at + 1;
            }
        }
        const struct pc_course_rule *rule = count >= 42 ?
            find_rule(number(fields[2]), number(fields[4])) : NULL;
        size_t row_start = written;
        for (size_t col = 0; col < count; ++col) {
            char value[16];
            const char *text = fields[col].text;
            size_t length = fields[col].size;
            if (rule && col >= 11 && col <= 38) {
                length = (size_t)snprintf(value, sizeof(value), "%u", rule->values[col - 11]);
                text = value;
            }
            if (written + length + 2 > capacity) goto invalid;
            memcpy(output + written, text, length);
            written += length;
            if (col + 1 < count) output[written++] = ',';
        }
        /* DataSetNew has a 512-byte row scratch buffer. */
        if (rule && written - row_start >= 512) goto invalid;
        while (end < size && (data[end] == '\r' || data[end] == '\n')) {
            if (written + 1 >= capacity) goto invalid;
            output[written++] = data[end++];
        }
        start = end;
    }
    output[written] = '\0';
    *output_size = written;
    return output;
invalid:
    free(output);
    return NULL;
}
