/*
 * Stand-in for the game: asks Windows for the local addresses in the ways a
 * game might, then binds sockets and reports where they really ended up.
 *
 *   testapp.exe <ip to bind to>           run the checks
 *   testapp.exe <ip to bind to> --spawn   run them in a child process instead
 *                                         (like CNC3.exe starting cnc3game.dat)
 *   testapp.exe <ip to bind to> --sleep   wait 3 seconds first, to give
 *                                         "portrebind --wait" time to find us
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void report_socket(const char *what, SOCKET s)
{
    struct sockaddr_in local;
    int len = sizeof local;

    if (getsockname(s, (struct sockaddr *)&local, &len) == 0)
        printf("%-28s %s\n", what, inet_ntoa(local.sin_addr));
    else
        printf("%-28s error %d\n", what, WSAGetLastError());
}

static void bind_and_report(const char *what, int type, const char *ip, int port)
{
    SOCKET s = socket(AF_INET, type, 0);
    struct sockaddr_in addr = { 0 };

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip);
    addr.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0)
        printf("%-28s bind error %d\n", what, WSAGetLastError());
    else
        report_socket(what, s);
    closesocket(s);
}

int main(int argc, char **argv)
{
    const char *bind_ip = argc > 1 ? argv[1] : "0.0.0.0";
    char name[256], buf[8192];
    struct hostent *host;
    struct addrinfo hints = { 0 }, *info, *ai;
    IP_ADAPTER_INFO *adapter = (IP_ADAPTER_INFO *)buf;
    MIB_IPADDRTABLE *table = (MIB_IPADDRTABLE *)buf;
    struct sockaddr_in dest = { 0 };
    ULONG size;
    WSADATA wsa;
    SOCKET s;
    DWORD i;

    /* portrebind.exe is a windowed program and does not hand its stdout down
     * to us, so the test names a file to report into instead. */
    if (getenv("TESTAPP_OUT"))
        freopen(getenv("TESTAPP_OUT"), "a", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc > 2 && strcmp(argv[2], "--spawn") == 0) {
        STARTUPINFOA startup = { sizeof startup };
        PROCESS_INFORMATION child;
        char cmd[600];

        snprintf(cmd, sizeof cmd, "\"%s\" %s", argv[0], bind_ip);
        if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &child)) {
            printf("CreateProcess failed: %lu\n", GetLastError());
            return 1;
        }
        WaitForSingleObject(child.hProcess, INFINITE);
        return 0;
    }

    if (argc > 2 && strcmp(argv[2], "--sleep") == 0)
        Sleep(3000);

    WSAStartup(MAKEWORD(2, 2), &wsa);

    gethostname(name, sizeof name);
    printf("%-28s", "gethostbyname:");
    host = gethostbyname(name);
    for (i = 0; host && host->h_addr_list[i]; i++)
        printf(" %s", inet_ntoa(*(struct in_addr *)host->h_addr_list[i]));
    printf("\n");

    printf("%-28s", "getaddrinfo:");
    hints.ai_family = AF_INET;
    if (getaddrinfo(name, NULL, &hints, &info) == 0) {
        for (ai = info; ai; ai = ai->ai_next)
            printf(" %s", inet_ntoa(((struct sockaddr_in *)ai->ai_addr)->sin_addr));
        freeaddrinfo(info);
    }
    printf("\n");

    printf("%-28s", "GetAdaptersInfo:");
    size = sizeof buf;
    if (GetAdaptersInfo(adapter, &size) == ERROR_SUCCESS) {
        for (; adapter; adapter = adapter->Next) {
            IP_ADDR_STRING *ip;
            for (ip = &adapter->IpAddressList; ip; ip = ip->Next)
                printf(" %s", ip->IpAddress.String);
        }
    }
    printf("\n");

    printf("%-28s", "GetIpAddrTable:");
    size = sizeof buf;
    if (GetIpAddrTable(table, &size, FALSE) == NO_ERROR)
        for (i = 0; i < table->dwNumEntries; i++)
            printf(" %s", inet_ntoa(*(struct in_addr *)&table->table[i].dwAddr));
    printf("\n");

    bind_and_report("udp bind to given ip:", SOCK_DGRAM, bind_ip, 8086);
    bind_and_report("udp bind to 0.0.0.0:", SOCK_DGRAM, "0.0.0.0", 8087);
    bind_and_report("udp bind to 127.0.0.1:", SOCK_DGRAM, "127.0.0.1", 8088);
    bind_and_report("tcp bind to given ip:", SOCK_STREAM, bind_ip, 8089);
    bind_and_report("tcp bind to 0.0.0.0:", SOCK_STREAM, "0.0.0.0", 8090);

    s = socket(AF_INET, SOCK_DGRAM, 0);
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = inet_addr("192.0.2.1"); /* TEST-NET, goes nowhere */
    dest.sin_port = htons(9);
    sendto(s, "x", 1, 0, (struct sockaddr *)&dest, sizeof dest);
    report_socket("udp sendto without bind:", s);
    closesocket(s);

    return 0;
}
