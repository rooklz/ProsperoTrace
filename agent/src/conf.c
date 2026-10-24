#include "conf.h"

#include <stdlib.h>
#include <string.h>

/* returned values are heap copies, valid for the life of the process */
const char *pt_conf_get(const char *text, const char *key)
{
    size_t klen = strlen(key);
    for (const char *line = text; *line; ) {
        while (*line == ' ' || *line == '\t')
            line++;
        size_t len = strcspn(line, "\n");
        if (len > klen && strncmp(line, key, klen) == 0 && line[klen] == '=') {
            const char *val = line + klen + 1;
            size_t vlen = len - klen - 1;
            while (vlen && (val[vlen - 1] == '\r' || val[vlen - 1] == ' '))
                vlen--;
            char *out = malloc(vlen + 1);
            if (!out)
                return 0;
            memcpy(out, val, vlen);
            out[vlen] = 0;
            return out;
        }
        line += len;
        if (*line == '\n')
            line++;
    }
    return 0;
}
