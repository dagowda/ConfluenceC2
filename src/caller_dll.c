/* Author: Dhanush Arvind */

#define BUNDLED_BUILD

#include "caller.c"

static DWORD WINAPI caller_thread(LPVOID param) {
    (void)param;
    if (check_debugger()) return 1;
    c2_main();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL;
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        CreateThread(NULL, 0, caller_thread, NULL, 0, NULL);
    }
    return TRUE;
}
