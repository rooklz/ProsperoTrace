#include "nids.h"

#include <string.h>

/* table is sorted by nid (see tools/nidgen.py) */
const struct nid_entry *nid_lookup(uint64_t nid)
{
    unsigned long lo = 0, hi = g_nid_table_count;
    while (lo < hi) {
        unsigned long mid = lo + (hi - lo) / 2;
        if (g_nid_table[mid].nid < nid)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < g_nid_table_count && g_nid_table[lo].nid == nid)
        return &g_nid_table[lo];
    return 0;
}

/* payload imports carry plain names; a linear scan is fine at install time */
const struct nid_entry *nid_lookup_name(const char *fn)
{
    for (unsigned long i = 0; i < g_nid_table_count; i++)
        if (strcmp(g_nid_table[i].fn, fn) == 0)
            return &g_nid_table[i];
    return 0;
}
