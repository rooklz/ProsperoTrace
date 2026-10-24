#pragma once

#include <stddef.h>
#include <stdint.h>

struct nid_entry {
    uint64_t nid;
    const char *mod;
    const char *fn;
};

extern const struct nid_entry g_nid_table[];
extern const unsigned long g_nid_table_count;

const struct nid_entry *nid_lookup(uint64_t nid);
