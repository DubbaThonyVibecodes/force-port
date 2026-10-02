/*
 * portrebind.dll - lives inside the game process and corrects which network
 * interface the game ends up on.
 *
 * It patches a handful of Windows API functions (via MinHook, which overwrites
 * the first bytes of each function with a jump to ours and hands back a
 * "real_xxx" pointer that still reaches the original):
 *
 *   bind                    the fix itself: a bind to the wrong local address
 *                           is redirected to the configured one
 *   sendto, connect         sockets used without bind() get bound by us first
 *   gethostbyname,          the ways a game can ask "what are my addresses?";
 *   getaddrinfo,            we answer with the configured one only, so the
 *   GetAdaptersInfo,        game also *believes* (and tells other players)
 *   GetIpAddrTable          the right address
 *   CreateProcessInternalW  every child process gets this DLL as well, because
 *                           CNC3.exe is only a launcher for cnc3game.dat
 */
#include "common.h"
#include "inject.h"
#include <MinHook.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static struct config g_cfg;
static wchar_t g_dll_path[MAX_PATH];
static HANDLE g_log = INVALID_HANDLE_VALUE;
static DWORD g_target; /* the address to force; 0 until resolved */

/* The unpatched originals. MinHook fills these in. */
static int (WSAAPI *real_bind)(SOCKET, const struct sockaddr *, int);
static int (WSAAPI *real_sendto)(SOCKET, const char *, int, int, const struct sockaddr *, int);
static int (WSAAPI *real_connect)(SOCKET, const struct sockaddr *, int);
static struct hostent *(WSAAPI *real_gethostbyname)(const char *);
static int (WSAAPI *real_getaddrinfo)(const char *, const char *, const struct addrinfo *,
                                      struct addrinfo **);
static ULONG (WINAPI *real_GetAdaptersInfo)(PIP_ADAPTER_INFO, PULONG);
static DWORD (WINAPI *real_GetIpAddrTable)(PMIB_IPADDRTABLE, PULONG, BOOL);
static BOOL (WINAPI *real_CreateProcessInternalW)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
                                                  LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
                                                  LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION,
                                                  PHANDLE);

/* ---------------------------------------------------------------- logging */

/* Appends one line to portrebind.log. Keeps the thread's last-error value
 * intact, since the game reads it right after the calls we sit in.
 * The format is a wide string: use %ls for wide and %hs for narrow strings. */
static void log_line(const wchar_t *fmt, ...)
{
    wchar_t wide[600];
    char line[3 * 600 + 2]; /* UTF-8 needs up to 3 bytes per wide char */
    DWORD saved_error = GetLastError(), written;
    SYSTEMTIME now;
    va_list args;
    int len, n;

    if (g_log == INVALID_HANDLE_VALUE)
        return;

    GetLocalTime(&now);
    len = _snwprintf(wide, ARRAYSIZE(wide), L"%02u:%02u:%02u.%03u [pid %lu] ", now.wHour,
                     now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId());
    va_start(args, fmt);
    n = _vsnwprintf(wide + len, ARRAYSIZE(wide) - len, fmt, args);
    va_end(args);
    len = (n < 0) ? (int)ARRAYSIZE(wide) : len + n; /* n < 0: the line was cut short */

    len = WideCharToMultiByte(CP_UTF8, 0, wide, len, line, sizeof line - 2, NULL, NULL);
    line[len++] = '\r';
    line[len++] = '\n';

    /* The file is opened in append mode, so each write lands at the end even
     * with several processes logging into it. */
    WriteFile(g_log, line, len, &written, NULL);
    SetLastError(saved_error);
}

struct ip_text { char s[16]; };

static struct ip_text ip_str(DWORD addr)
{
    const BYTE *b = (const BYTE *)&addr;
    struct ip_text text;

    snprintf(text.s, sizeof text.s, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return text;
}

/* ----------------------------------------------------------------- helpers */

/* Resolved on first use rather than at load time: a subnet-style setting needs
 * the interface list, and DllMain is no place for that kind of call. */
static DWORD target_ip(void)
{
    static BOOL complained;

    if (!g_target) {
        g_target = resolve_target_ip(g_cfg.ip, real_GetIpAddrTable);
        if (g_target) {
            log_line(L"forcing local address %hs", ip_str(g_target).s);
        } else if (!complained) {
            complained = TRUE;
            log_line(L"no local address matches ip=\"%ls\" yet; leaving calls untouched", g_cfg.ip);
        }
    }
    return g_target;
}

static int socket_type(SOCKET s)
{
    int type = 0, len = sizeof type;

    getsockopt(s, SOL_SOCKET, SO_TYPE, (char *)&type, &len);
    return type;
}

static const char *socket_kind(SOCKET s)
{
    int type = socket_type(s);

    return type == SOCK_DGRAM ? "udp" : type == SOCK_STREAM ? "tcp" : "other";
}

/* Does the rewrite_any setting cover this socket? */
static BOOL any_applies(SOCKET s)
{
    if (g_cfg.rewrite_any == ANY_ALL)
        return TRUE;
    return g_cfg.rewrite_any == ANY_UDP && socket_type(s) == SOCK_DGRAM;
}

static BOOL is_own_hostname(const char *name)
{
    char own[256];

    if (!*name)
        return TRUE;
    return gethostname(own, sizeof own) == 0 && lstrcmpiA(name, own) == 0;
}

/* ------------------------------------------------------------ socket hooks */

static int WSAAPI hook_bind(SOCKET s, const struct sockaddr *name, int namelen)
{
    const struct sockaddr_in *wanted = (const struct sockaddr_in *)name;
    struct sockaddr_in fixed;
    DWORD target = target_ip(), addr;
    BOOL rewrite;
    int ret;

    if (!name || namelen < (int)sizeof *wanted || wanted->sin_family != AF_INET)
        return real_bind(s, name, namelen); /* IPv6 and friends: not our business */

    addr = wanted->sin_addr.s_addr;
    if (!target || addr == target || is_loopback(addr))
        rewrite = FALSE;
    else if (addr == INADDR_ANY)
        rewrite = any_applies(s);
    else
        rewrite = TRUE; /* the bug: an explicit bind to the wrong interface */

    if (rewrite) {
        fixed = *wanted;
        fixed.sin_addr.s_addr = target;
        ret = real_bind(s, (const struct sockaddr *)&fixed, sizeof fixed);
        log_line(L"bind %hs %hs:%u -> %hs:%u, result %d (error %d)", socket_kind(s), ip_str(addr).s,
                 ntohs(wanted->sin_port), ip_str(target).s, ntohs(wanted->sin_port), ret,
                 ret ? WSAGetLastError() : 0);
        if (ret == 0)
            return 0;
        /* Our address was refused; better the game's own choice than nothing. */
    }

    ret = real_bind(s, name, namelen);
    log_line(L"bind %hs %hs:%u untouched, result %d (error %d)", socket_kind(s), ip_str(addr).s,
             ntohs(wanted->sin_port), ret, ret ? WSAGetLastError() : 0);
    return ret;
}

/* A socket that is used without bind() gets bound by Windows on the spot, to
 * whichever interface the routing table prefers. Beat Windows to it. */
static void bind_if_unbound(SOCKET s, const struct sockaddr *dest, int destlen, const char *caller)
{
    const struct sockaddr_in *to = (const struct sockaddr_in *)dest;
    struct sockaddr_in local;
    int len = sizeof local, ret;
    DWORD target;

    /* getsockname fails with WSAEINVAL on a socket that is not bound yet. */
    if (getsockname(s, (struct sockaddr *)&local, &len) == 0 || WSAGetLastError() != WSAEINVAL)
        return;
    if (!dest || destlen < (int)sizeof *to || to->sin_family != AF_INET ||
        is_loopback(to->sin_addr.s_addr))
        return;
    target = target_ip();
    if (!target || !any_applies(s))
        return;

    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = target;
    ret = real_bind(s, (const struct sockaddr *)&local, sizeof local);
    log_line(L"%hs to %hs:%u on unbound %hs socket: bound it to %hs first, result %d (error %d)",
             caller, ip_str(to->sin_addr.s_addr).s, ntohs(to->sin_port), socket_kind(s),
             ip_str(target).s, ret, ret ? WSAGetLastError() : 0);
}

static int WSAAPI hook_sendto(SOCKET s, const char *buf, int len, int flags,
                              const struct sockaddr *to, int tolen)
{
    bind_if_unbound(s, to, tolen, "sendto");
    return real_sendto(s, buf, len, flags, to, tolen);
}

static int WSAAPI hook_connect(SOCKET s, const struct sockaddr *name, int namelen)
{
    bind_if_unbound(s, name, namelen, "connect");
    return real_connect(s, name, namelen);
}

/* --------------------------------------------- "what are my addresses" hooks */

static struct hostent *WSAAPI hook_gethostbyname(const char *name)
{
    struct hostent *host = real_gethostbyname(name);
    DWORD target = target_ip();
    int count = 0;

    if (!host || !target || host->h_addrtype != AF_INET || host->h_length != 4 ||
        !host->h_addr_list || !host->h_addr_list[0])
        return host;
    if (name && !is_own_hostname(name)) /* NULL also means "this machine" */
        return host;

    while (host->h_addr_list[count])
        count++;
    /* The result lives in a per-thread buffer owned by winsock; cutting the
     * list down in place is fine. */
    memcpy(host->h_addr_list[0], &target, 4);
    host->h_addr_list[1] = NULL;
    log_line(L"gethostbyname(own name): %d address(es) replaced by %hs", count, ip_str(target).s);
    return host;
}

static int WSAAPI hook_getaddrinfo(const char *node, const char *service,
                                   const struct addrinfo *hints, struct addrinfo **result)
{
    int ret = real_getaddrinfo(node, service, hints, result);
    DWORD target = target_ip();
    struct addrinfo *ai;
    int count = 0;

    if (ret != 0 || !result || !target || !node || !is_own_hostname(node))
        return ret;

    /* Entries cannot be unlinked safely (freeaddrinfo owns the layout), so
     * every IPv4 entry is overwritten instead. Duplicates are harmless. */
    for (ai = *result; ai; ai = ai->ai_next) {
        struct sockaddr_in *in = (struct sockaddr_in *)ai->ai_addr;
        if (ai->ai_family != AF_INET || !in || ai->ai_addrlen < sizeof *in)
            continue;
        if (in->sin_addr.s_addr == INADDR_ANY || is_loopback(in->sin_addr.s_addr))
            continue;
        in->sin_addr.s_addr = target;
        count++;
    }
    log_line(L"getaddrinfo(own name): %d address(es) replaced by %hs", count, ip_str(target).s);
    return ret;
}

/* Leaves only the adapter that owns the target address, with only that address. */
static ULONG WINAPI hook_GetAdaptersInfo(PIP_ADAPTER_INFO info, PULONG size)
{
    ULONG ret = real_GetAdaptersInfo(info, size);
    DWORD target = target_ip();
    struct ip_text wanted = ip_str(target);
    IP_ADAPTER_INFO *adapter;
    IP_ADDR_STRING *ip, only;

    if (ret != ERROR_SUCCESS || !info || !target)
        return ret;

    for (adapter = info; adapter; adapter = adapter->Next) {
        for (ip = &adapter->IpAddressList; ip; ip = ip->Next) {
            if (strcmp(ip->IpAddress.String, wanted.s) != 0)
                continue;
            only = *ip;
            only.Next = NULL;
            /* The caller reads the list starting at the buffer's first entry,
             * so the adapter we keep has to be moved there. */
            if (adapter != info)
                *info = *adapter;
            info->Next = NULL;
            info->IpAddressList = only;
            log_line(L"GetAdaptersInfo: showing only the adapter with %hs", wanted.s);
            return ret;
        }
    }
    return ret;
}

/* Leaves only the target address (and loopback) in the table. */
static DWORD WINAPI hook_GetIpAddrTable(PMIB_IPADDRTABLE table, PULONG size, BOOL sorted)
{
    DWORD ret = real_GetIpAddrTable(table, size, sorted);
    DWORD target = target_ip(), i, kept = 0;
    BOOL present = FALSE;

    if (ret != NO_ERROR || !table || !target)
        return ret;

    for (i = 0; i < table->dwNumEntries; i++)
        if (table->table[i].dwAddr == target)
            present = TRUE;
    if (!present)
        return ret;

    for (i = 0; i < table->dwNumEntries; i++) {
        DWORD addr = table->table[i].dwAddr;
        if (addr == target || is_loopback(addr))
            table->table[kept++] = table->table[i];
    }
    log_line(L"GetIpAddrTable: kept %lu of %lu rows", kept, table->dwNumEntries);
    table->dwNumEntries = kept;
    return ret;
}

/* -------------------------------------------------------- child processes */

/* Every flavour of CreateProcess ends up in this undocumented function. The
 * child is started suspended, gets the DLL, and is then let go - unless the
 * caller wanted it suspended anyway. */
static BOOL WINAPI hook_CreateProcessInternalW(HANDLE token, LPCWSTR app, LPWSTR cmdline,
                                               LPSECURITY_ATTRIBUTES process_attrs,
                                               LPSECURITY_ATTRIBUTES thread_attrs, BOOL inherit,
                                               DWORD flags, LPVOID env, LPCWSTR cwd,
                                               LPSTARTUPINFOW startup, LPPROCESS_INFORMATION info,
                                               PHANDLE new_token)
{
    const char *step;
    DWORD err;

    if (!real_CreateProcessInternalW(token, app, cmdline, process_attrs, thread_attrs, inherit,
                                     flags | CREATE_SUSPENDED, env, cwd, startup, info, new_token))
        return FALSE;

    err = inject_dll(info->hProcess, g_dll_path, &step);
    if (err == ERROR_SUCCESS)
        log_line(L"child process %lu (%ls): injected", info->dwProcessId, app ? app : cmdline);
    else
        log_line(L"child process %lu (%ls): NOT injected, %hs failed with error %lu",
                 info->dwProcessId, app ? app : cmdline, step, err);

    if (!(flags & CREATE_SUSPENDED))
        ResumeThread(info->hThread);
    return TRUE;
}

/* ------------------------------------------------------------------ set-up */

static void hook(const wchar_t *module, const char *function, void *replacement, void *real)
{
    MH_STATUS status = MH_CreateHookApi(module, function, replacement, (LPVOID *)real);

    if (status != MH_OK)
        log_line(L"could not hook %ls!%hs: %hs", module, function, MH_StatusToString(status));
}

static void start(HINSTANCE dll)
{
    wchar_t path[MAX_PATH], exe[MAX_PATH] = L"?";
    MH_STATUS status;

    GetModuleFileNameW(dll, g_dll_path, ARRAYSIZE(g_dll_path));
    if (path_next_to_module(dll, INI_NAME, path, ARRAYSIZE(path)))
        config_load(path, &g_cfg);
    if (g_cfg.log && path_next_to_module(dll, LOG_NAME, path, ARRAYSIZE(path)))
        g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
    log_line(L"loaded into %ls (ip=%ls)", exe, g_cfg.ip);

    status = MH_Initialize();
    if (status != MH_OK) {
        log_line(L"MinHook failed to start: %hs", MH_StatusToString(status));
        return;
    }

    /* Used by target_ip(); stays the plain function if the hook is off. */
    real_GetIpAddrTable = GetIpAddrTable;

    hook(L"ws2_32", "bind", hook_bind, &real_bind);
    if (g_cfg.rewrite_any != ANY_NONE) {
        hook(L"ws2_32", "sendto", hook_sendto, &real_sendto);
        hook(L"ws2_32", "connect", hook_connect, &real_connect);
    }
    if (g_cfg.hide_other_ips) {
        hook(L"ws2_32", "gethostbyname", hook_gethostbyname, &real_gethostbyname);
        hook(L"ws2_32", "getaddrinfo", hook_getaddrinfo, &real_getaddrinfo);
        hook(L"iphlpapi", "GetAdaptersInfo", hook_GetAdaptersInfo, &real_GetAdaptersInfo);
        hook(L"iphlpapi", "GetIpAddrTable", hook_GetIpAddrTable, &real_GetIpAddrTable);
    }
    if (g_cfg.follow_children) {
        /* Windows 8 and later keep it in kernelbase, Windows 7 in kernel32. */
        if (MH_CreateHookApi(L"kernelbase", "CreateProcessInternalW", hook_CreateProcessInternalW,
                             (LPVOID *)&real_CreateProcessInternalW) != MH_OK)
            hook(L"kernel32", "CreateProcessInternalW", hook_CreateProcessInternalW,
                 &real_CreateProcessInternalW);
    }

    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK)
        log_line(L"could not enable hooks: %hs", MH_StatusToString(status));
}

BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(dll);
        start(dll);
    }
    return TRUE;
}
