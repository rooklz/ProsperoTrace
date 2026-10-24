#include "jsonesc.h"

#include <stdio.h>

size_t pt_json_escape(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    if (!cap)
        return 0;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p; p++) {
        char esc[7];
        size_t n;
        if (*p == '"' || *p == '\\') {
            esc[0] = '\\'; esc[1] = (char)*p; n = 2;
        } else if (*p < 0x20) {
            n = (size_t)snprintf(esc, sizeof(esc), "\\u%04x", *p);
        } else {
            esc[0] = (char)*p; n = 1;
        }
        if (o + n >= cap)
            break;
        for (size_t i = 0; i < n; i++)
            dst[o++] = esc[i];
    }
    dst[o] = 0;
    return o;
}
