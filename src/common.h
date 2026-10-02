/* Bits shared by the launcher (portrebind.exe) and the injected DLL. */
#ifndef PORTREBIND_COMMON_H
#define PORTREBIND_COMMON_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#define INI_NAME    L"portrebind.ini"
#define DLL_NAME    L"portrebind.dll"
#define LOG_NAME    L"portrebind.log"
#define INI_SECTION L"portrebind"

/* What to do with sockets the game does not bind to any specific address. */
enum rewrite_any { ANY_NONE, ANY_UDP, ANY_ALL };

struct config {
    wchar_t ip[64];         /* "192.168.1.10" or a subnet, "192.168.1.0/24" */
    wchar_t game[MAX_PATH]; /* what the launcher starts when given no arguments */
    wchar_t args[1024];
    enum rewrite_any rewrite_any;
    BOOL hide_other_ips;
    BOOL follow_children;
    BOOL log;
    BOOL ask;               /* launcher only: show the address dialog at start */
};

/* All addresses in this project are IPv4 in network byte order, as winsock
 * hands them out. On x86 that puts the first octet in the lowest byte. */
static inline BOOL is_loopback(DWORD addr)
{
    return (addr & 0xFF) == 127;
}

/* Writes "<directory of module>\<name>" to out. NULL module = the running exe. */
BOOL path_next_to_module(HMODULE module, const wchar_t *name, wchar_t *out, DWORD out_len);

void config_load(const wchar_t *ini_path, struct config *cfg);

/* The DLL hooks GetIpAddrTable, so it has to pass in the unhooked original. */
typedef DWORD (WINAPI *GetIpAddrTable_fn)(PMIB_IPADDRTABLE, PULONG, BOOL);

/* Returns a HeapAlloc'ed table of this machine's addresses, or NULL. */
MIB_IPADDRTABLE *ip_table_get(GetIpAddrTable_fn get_table);

BOOL ip_is_local(DWORD addr, GetIpAddrTable_fn get_table);

/* Turns the "ip" setting into an address. A plain address is returned as is,
 * a subnet is resolved to the local address inside it. Returns 0 on failure. */
DWORD resolve_target_ip(const wchar_t *spec, GetIpAddrTable_fn get_table);

#endif
