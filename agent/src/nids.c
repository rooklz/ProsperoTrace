#include "nids.h"

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
