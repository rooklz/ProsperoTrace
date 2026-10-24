#pragma once
#include <stddef.h>

/* Copy src into dst (capacity cap, always NUL-terminated) as the body of a
 * JSON string: quotes, backslashes and control bytes are escaped. Output
 * that would not fit is cut at a character boundary, never mid-escape. */
size_t pt_json_escape(char *dst, size_t cap, const char *src);
