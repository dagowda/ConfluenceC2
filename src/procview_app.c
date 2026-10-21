/* Author: Dhanush Arvind */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>

#pragma comment(lib, "psapi.lib")

int procview_main(void) {
    printf("============================================================\n");
    printf("  QuickProcView v1.0 - Windows Process Viewer\n");
    printf("============================================================\n\n");

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[!] CreateToolhelp32Snapshot failed: %lu\n", GetLastError());
        return 1;
    }

    printf("  %-8s %-8s %-30s %s\n", "PID", "PPID", "Process Name", "Memory (KB)");
    printf("  %-8s %-8s %-30s %s\n", "--------", "--------",
           "------------------------------", "-----------");

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    int count = 0;

    if (!Process32First(hSnap, &pe)) {
        CloseHandle(hSnap);
        fprintf(stderr, "[!] Process32First failed\n");
        return 1;
    }

    do {
        char memStr[32];
        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                   FALSE, pe.th32ProcessID);
        if (hProc) {
            PROCESS_MEMORY_COUNTERS pmc;
            pmc.cb = sizeof(pmc);
            if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                _snprintf(memStr, sizeof(memStr), "%lu",
                         (unsigned long)(pmc.WorkingSetSize / 1024));
            } else {
                _snprintf(memStr, sizeof(memStr), "-");
            }
            CloseHandle(hProc);
        } else {
            _snprintf(memStr, sizeof(memStr), "-");
        }
        memStr[sizeof(memStr) - 1] = '\0';

        printf("  %-8lu %-8lu %-30s %s\n",
               (unsigned long)pe.th32ProcessID,
               (unsigned long)pe.th32ParentProcessID,
               pe.szExeFile,
               memStr);
        count++;
    } while (Process32Next(hSnap, &pe));

    CloseHandle(hSnap);

    printf("\n  Total processes: %d\n", count);
    printf("============================================================\n");

    return 0;
}

#ifndef BUNDLED_BUILD
int main(void) {
    return procview_main();
}
#endif
