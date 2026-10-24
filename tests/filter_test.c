/* filter_test.c — host test for the hook selection filter. */
#include <assert.h>
#include <stdio.h>

#include "../agent/src/filter.h"

#define CHECK(pat, str, want) \
    assert(pt_glob_match(pat, str) == (want)); \
    (void)0

static void test_glob(void)
{
    CHECK("*", "anything", 1);
    CHECK("", "", 1);
    CHECK("", "x", 0);
    CHECK("abc", "abc", 1);
    CHECK("abc", "abcd", 0);
    CHECK("libkernel.*", "libkernel.sceKernelOpen", 1);
    CHECK("libkernel.*", "libSceLibcInternal.malloc", 0);
    CHECK("*.sceKernelOpen", "libkernel.sceKernelOpen", 1);
    CHECK("libSceVideoOut.sc?Quit", "libSceVideoOut.scrQuit", 1);
    CHECK("libSceVideoOut.sc?Quit", "libSceVideoOut.scQuit", 0);
    CHECK("a*b*c", "aXBbXc", 1);
    CHECK("a*b*c", "abc", 1);
    CHECK("a*b*c", "ac", 0);
}

static void test_filter(void)
{
    assert(pt_filter_is_set() == 0);
    assert(pt_filter_allows("libkernel", "sceKernelOpen") == 1);  /* unset = allow all */

    assert(pt_set_filter("libkernel.sceKernelOpen,libSceVideoOut.*") == 0);
    assert(pt_filter_is_set() == 1);
    assert(pt_filter_allows("libkernel", "sceKernelOpen") == 1);
    assert(pt_filter_allows("libkernel", "sceKernelClose") == 0);
    assert(pt_filter_allows("libSceVideoOut", "sceVideoOutOpen") == 1);
    assert(pt_filter_allows("libSceAudioOut", "sceAudioOutOpen") == 0);

    assert(pt_set_filter("*") == 0);
    assert(pt_filter_allows("libkernel", "sceKernelOpen") == 1);

    assert(pt_set_filter("") == 0);
    assert(pt_filter_is_set() == 0);
    assert(pt_filter_allows("libkernel", "sceKernelOpen") == 1);
}

int main(void)
{
    test_glob();
    test_filter();
    puts("filter_test: OK");
    return 0;
}