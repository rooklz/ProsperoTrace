#pragma once

/* Value of key in KEY=VALUE text (one per line; leading blanks and a
 * trailing CR or spaces are ignored). Returns a heap copy, or 0. */
const char *pt_conf_get(const char *text, const char *key);
