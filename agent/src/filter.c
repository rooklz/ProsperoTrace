#include "filter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PT_FILTER_MAX 64

static const char *g_patterns[PT_FILTER_MAX];
static size_t g_pattern_count;
static char *g_filter_owned;

int pt_glob_match(const char *pat, const char *s)
{
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat)
                return 1;
            while (*s) {
                if (pt_glob_match(pat, s++))
                    return 1;
            }
            return 0;
        } else if (*pat == '?') {
            if (!*s)
                return 0;
            pat++;
            s++;
        } else {
            if (*s != *pat)
                return 0;
            pat++;
            s++;
        }
    }
    return *s == 0;
}

int pt_filter_is_set(void)
{
    return g_pattern_count > 0;
}

int pt_set_filter(const char *patterns)
{
    free(g_filter_owned);
    g_filter_owned = strdup(patterns ? patterns : "");
    if (!g_filter_owned)
        return -1;

    g_pattern_count = 0;
    char *save = 0;
    for (char *tok = strtok_r(g_filter_owned, ",", &save);
         tok && g_pattern_count < PT_FILTER_MAX;
         tok = strtok_r(0, ",", &save)) {
        while (*tok == ' ' || *tok == '\t')
            tok++;
        size_t n = strlen(tok);
        while (n && (tok[n - 1] == ' ' || tok[n - 1] == '\t' || tok[n - 1] == '\n'))
            tok[--n] = 0;
        if (!*tok)
            continue;
        g_patterns[g_pattern_count++] = tok;
    }
    return 0;
}

int pt_filter_allows(const char *mod, const char *fn)
{
    if (!pt_filter_is_set())
        return 1;   /* no filter: hook everything we know */

    char modfn[192];
    snprintf(modfn, sizeof(modfn), "%s.%s", mod, fn);
    for (size_t i = 0; i < g_pattern_count; i++)
        if (pt_glob_match(g_patterns[i], modfn))
            return 1;
    return 0;
}