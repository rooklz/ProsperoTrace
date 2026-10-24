#include "../agent/src/conf.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *t = "PT_HOST=1.2.3.4\r\n  PT_PID_FILE=/data/ptdemo.pid\nPT_WAIT=20 \n# PT_PID=1\nPT_PID_FILEX=no";
    assert(strcmp(pt_conf_get(t, "PT_HOST"), "1.2.3.4") == 0);
    assert(strcmp(pt_conf_get(t, "PT_PID_FILE"), "/data/ptdemo.pid") == 0);
    assert(strcmp(pt_conf_get(t, "PT_WAIT"), "20") == 0);
    assert(pt_conf_get(t, "PT_PID") == 0);
    assert(pt_conf_get(t, "PT_NAME") == 0);
    assert(pt_conf_get("", "PT_HOST") == 0);
    puts("conf_test: OK");
    return 0;
}
