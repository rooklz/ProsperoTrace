#include "../agent/src/jsonesc.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void check(const char *in, size_t cap, const char *want)
{
    char out[64];
    pt_json_escape(out, cap, in);
    if (strcmp(out, want) != 0) {
        printf("FAIL: escape(%s, %zu) = [%s], want [%s]\n", in, cap, out, want);
        fails++;
    }
}

int main(void)
{
    check("ftpsrv", 64, "ftpsrv");
    check("say \"hi\"", 64, "say \\\"hi\\\"");
    check("a\\b", 64, "a\\\\b");
    check("tab\there", 64, "tab\\u0009here");
    check("ab\"cd", 4, "ab");
    check("abcdef", 4, "abc");
    check(NULL, 64, "");
    if (fails)
        return 1;
    printf("jsonesc_test: OK\n");
    return 0;
}
