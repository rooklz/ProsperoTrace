/* Host-side emitter harness: writes the emitted trampoline to stdout.
 * Used by tests/check_trampoline.sh to compare against the reference
 * assembly. Values match tests/trampoline_ref.s exactly. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../agent/src/trampoline.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    static uint8_t out[PT_TRAMPOLINE_MAX];
    size_t len = 0;
    if (trampoline_emit(0x1122334455667788ull, 0x8877665544332211ull, 7,
                        out, sizeof(out), &len) != 0) {
        fprintf(stderr, "emit failed\n");
        return 1;
    }
    if (fwrite(out, 1, len, stdout) != len)
        return 1;
    fprintf(stderr, "emitted %zu bytes\n", len);
    return 0;
}