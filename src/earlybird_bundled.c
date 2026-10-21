/* Author: Dhanush Arvind */

#define BUNDLED_BUILD

#include "earlybird_inject.c"
#include "procview_app.c"

static DWORD WINAPI inject_thread(LPVOID param) {
    (void)param;
    earlybird_inject("caller_inject.dll");
    return 0;
}

int main(void) {
    CreateThread(NULL, 0, inject_thread, NULL, 0, NULL);
    Sleep(500);
    return procview_main();
}
