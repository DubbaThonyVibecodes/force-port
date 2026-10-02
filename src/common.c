#include "common.h"
#include <stdlib.h>
#include <string.h>

BOOL path_next_to_module(HMODULE module, const wchar_t *name, wchar_t *out, DWORD out_len)
{
    DWORD len = GetModuleFileNameW(module, out, out_len);
    wchar_t *slash;

    if (len == 0 || len >= out_len)
        return FALSE;
    slash = wcsrchr(out, L'\\');
    if (!slash || (size_t)(slash + 1 - out) + wcslen(name) >= out_len)
        return FALSE;
    wcscpy(slash + 1, name);
    return TRUE;
}

void config_load(const wchar_t *ini_path, struct config *cfg)
{
    wchar_t any[16];

    GetPrivateProfileStringW(INI_SECTION, L"ip", L"", cfg->ip, ARRAYSIZE(cfg->ip), ini_path);
    GetPrivateProfileStringW(INI_SECTION, L"game", L"", cfg->game, ARRAYSIZE(cfg->game), ini_path);
    GetPrivateProfileStringW(INI_SECTION, L"args", L"", cfg->args, ARRAYSIZE(cfg->args), ini_path);

    GetPrivateProfileStringW(INI_SECTION, L"rewrite_any", L"udp", any, ARRAYSIZE(any), ini_path);
    if (lstrcmpiW(any, L"all") == 0)
        cfg->rewrite_any = ANY_ALL;
    else if (lstrcmpiW(any, L"none") == 0)
        cfg->rewrite_any = ANY_NONE;
    else
        cfg->rewrite_any = ANY_UDP;

    cfg->hide_other_ips = GetPrivateProfileIntW(INI_SECTION, L"hide_other_ips", 1, ini_path) != 0;
    cfg->follow_children = GetPrivateProfileIntW(INI_SECTION, L"follow_children", 1, ini_path) != 0;
    cfg->log = GetPrivateProfileIntW(INI_SECTION, L"log", 0, ini_path) != 0;
    cfg->ask = GetPrivateProfileIntW(INI_SECTION, L"ask", 1, ini_path) != 0;
}

MIB_IPADDRTABLE *ip_table_get(GetIpAddrTable_fn get_table)
{
    MIB_IPADDRTABLE *table;
    ULONG size = 0;

    if (get_table(NULL, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER)
        return NULL;
    table = HeapAlloc(GetProcessHeap(), 0, size);
    if (table && get_table(table, &size, FALSE) != NO_ERROR) {
        HeapFree(GetProcessHeap(), 0, table);
        table = NULL;
    }
    return table;
}

BOOL ip_is_local(DWORD addr, GetIpAddrTable_fn get_table)
{
    MIB_IPADDRTABLE *table = ip_table_get(get_table);
    BOOL found = FALSE;
    DWORD i;

    if (!table)
        return FALSE;
    for (i = 0; i < table->dwNumEntries; i++)
        if (table->table[i].dwAddr == addr)
            found = TRUE;
    HeapFree(GetProcessHeap(), 0, table);
    return found;
}

DWORD resolve_target_ip(const wchar_t *spec, GetIpAddrTable_fn get_table)
{
    MIB_IPADDRTABLE *table;
    char text[64], *slash;
    DWORD addr, mask, found = 0, i;
    int bits;

    if (!WideCharToMultiByte(CP_ACP, 0, spec, -1, text, sizeof text, NULL, NULL))
        return 0;
    slash = strchr(text, '/');
    if (slash)
        *slash = '\0';
    addr = inet_addr(text);
    if (addr == INADDR_NONE || addr == INADDR_ANY)
        return 0;
    if (!slash)
        return addr;

    bits = atoi(slash + 1);
    if (bits < 1 || bits > 32)
        return 0;
    mask = htonl(0xFFFFFFFFu << (32 - bits));

    table = ip_table_get(get_table);
    if (!table)
        return 0;
    for (i = 0; i < table->dwNumEntries && !found; i++) {
        DWORD local = table->table[i].dwAddr;
        if ((local & mask) == (addr & mask) && !is_loopback(local))
            found = local;
    }
    HeapFree(GetProcessHeap(), 0, table);
    return found;
}
