/* Author: Dhanush Arvind */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <iphlpapi.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

static void print_banner(void) {
    printf("\n");
    printf("  =============================================\n");
    printf("    QuickSysInfo  v2.1.0  -  System Viewer\n");
    printf("  =============================================\n\n");
}

static void print_identity(void) {
    char hostname[256] = {0}, username[256] = {0};
    DWORD hl = sizeof(hostname), ul = sizeof(username);
    GetComputerNameA(hostname, &hl);
    GetUserNameA(username, &ul);
    printf("  [*] Computer Name : %s\n", hostname);
    printf("  [*] Current User  : %s\n", username);
}

static void print_os_info(void) {
    OSVERSIONINFOEXA ovi;
    memset(&ovi, 0, sizeof(ovi));
    ovi.dwOSVersionInfoSize = sizeof(ovi);
#pragma warning(suppress: 4996)
    GetVersionExA((OSVERSIONINFOA *)&ovi);

    const char *edition = "";
    if (ovi.wProductType == VER_NT_WORKSTATION) edition = "Workstation";
    else if (ovi.wProductType == VER_NT_SERVER) edition = "Server";
    else edition = "Domain Controller";

    printf("  [*] OS Version    : Windows %lu.%lu Build %lu (%s)\n",
           ovi.dwMajorVersion, ovi.dwMinorVersion, ovi.dwBuildNumber, edition);
    if (ovi.szCSDVersion[0])
        printf("  [*] Service Pack  : %s\n", ovi.szCSDVersion);
}

static void print_uptime(void) {
    ULONGLONG ms = GetTickCount64();
    ULONGLONG sec = ms / 1000;
    ULONGLONG min = sec / 60;
    ULONGLONG hr = min / 60;
    ULONGLONG days = hr / 24;
    printf("  [*] Uptime        : %llud %lluh %llum %llus\n",
           days, hr % 24, min % 60, sec % 60);
}

static void print_cpu_info(void) {
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    const char *arch = "Unknown";
    switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: arch = "x64 (AMD64)"; break;
        case PROCESSOR_ARCHITECTURE_INTEL: arch = "x86 (i386)"; break;
        case PROCESSOR_ARCHITECTURE_ARM64: arch = "ARM64"; break;
    }
    printf("  [*] Architecture  : %s\n", arch);
    printf("  [*] Processors    : %lu logical cores\n", si.dwNumberOfProcessors);
}

static void print_memory(void) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    printf("  [*] Physical RAM  : %llu MB total, %llu MB free (%lu%% used)\n",
           ms.ullTotalPhys / (1024 * 1024),
           ms.ullAvailPhys / (1024 * 1024),
           ms.dwMemoryLoad);
    printf("  [*] Virtual Mem   : %llu MB total, %llu MB free\n",
           ms.ullTotalVirtual / (1024 * 1024),
           ms.ullAvailVirtual / (1024 * 1024));
}

static void print_network(void) {
    ULONG bufLen = 15000;
    IP_ADAPTER_ADDRESSES *addrs = (IP_ADAPTER_ADDRESSES *)malloc(bufLen);
    if (!addrs) return;

    ULONG flags = GAA_FLAG_INCLUDE_PREFIX;
    if (GetAdaptersAddresses(AF_UNSPEC, flags, NULL, addrs, &bufLen) == ERROR_BUFFER_OVERFLOW) {
        free(addrs);
        addrs = (IP_ADAPTER_ADDRESSES *)malloc(bufLen);
        if (!addrs) return;
    }

    if (GetAdaptersAddresses(AF_UNSPEC, flags, NULL, addrs, &bufLen) == NO_ERROR) {
        IP_ADAPTER_ADDRESSES *cur = addrs;
        int idx = 0;
        while (cur && idx < 8) {
            if (cur->OperStatus == IfOperStatusUp && cur->IfType != IF_TYPE_SOFTWARE_LOOPBACK) {
                printf("  [*] Adapter %-2d    : %S\n", idx, cur->FriendlyName);
                IP_ADAPTER_UNICAST_ADDRESS *ua = cur->FirstUnicastAddress;
                while (ua) {
                    char ip[64] = {0};
                    DWORD ipLen = sizeof(ip);
                    struct sockaddr *sa = ua->Address.lpSockaddr;
                    if (sa->sa_family == AF_INET) {
                        struct sockaddr_in *sin = (struct sockaddr_in *)sa;
                        sprintf(ip, "%d.%d.%d.%d",
                                sin->sin_addr.S_un.S_un_b.s_b1,
                                sin->sin_addr.S_un.S_un_b.s_b2,
                                sin->sin_addr.S_un.S_un_b.s_b3,
                                sin->sin_addr.S_un.S_un_b.s_b4);
                        printf("                    : IPv4 %s\n", ip);
                    }
                    ua = ua->Next;
                }
                idx++;
            }
            cur = cur->Next;
        }
    }
    free(addrs);
}

static void print_drives(void) {
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (drives & (1 << i)) {
            char root[4] = {'A' + i, ':', '\\', 0};
            UINT type = GetDriveTypeA(root);
            if (type == DRIVE_FIXED || type == DRIVE_REMOTE) {
                ULARGE_INTEGER total, avail;
                if (GetDiskFreeSpaceExA(root, &avail, &total, NULL)) {
                    printf("  [*] Drive %c:       : %llu GB total, %llu GB free\n",
                           'A' + i,
                           total.QuadPart / (1024ULL * 1024 * 1024),
                           avail.QuadPart / (1024ULL * 1024 * 1024));
                }
            }
        }
    }
}

int sysinfo_main(void) {
    print_banner();
    printf("  --- IDENTITY ---\n");
    print_identity();
    printf("\n  --- OPERATING SYSTEM ---\n");
    print_os_info();
    print_uptime();
    printf("\n  --- HARDWARE ---\n");
    print_cpu_info();
    print_memory();
    printf("\n  --- NETWORK ---\n");
    print_network();
    printf("\n  --- STORAGE ---\n");
    print_drives();
    printf("\n  =============================================\n");
    printf("  Press Enter to exit...\n");
    getchar();
    return 0;
}

#ifndef BUNDLED_BUILD
int main(void) {
    return sysinfo_main();
}
#endif
