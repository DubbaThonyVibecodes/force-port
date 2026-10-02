/*
 * portrebind.exe - starts the game with portrebind.dll loaded into it.
 *
 *   portrebind.exe                         start the "game" from portrebind.ini
 *   portrebind.exe <game.exe> [args...]    start this instead
 *   portrebind.exe --wait [process name]   inject into a game started elsewhere
 *   portrebind.exe --list                  show this machine's addresses
 *
 * Built as a windowed program, so double-clicking it does not flash a console.
 * Messages go to the terminal it was started from if there is one, and to a
 * message box if there is not.
 */
#include "common.h"
#include "dialog.h"
#include "inject.h"
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <tlhelp32.h>

#define DEFAULT_WAIT_NAME L"cnc3game.dat"
#define TITLE             L"portrebind"

/* ------------------------------------------------------------------ output */

static HANDLE g_out;         /* where say() prints; NULL = nowhere to print */
static BOOL g_own_console;   /* the console is ours and closes when we exit */
static wchar_t g_said[4096]; /* everything said so far, for the message box */

static void use_console(void)
{
    g_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (g_out == INVALID_HANDLE_VALUE)
        g_out = NULL;
}

static void output_init(void)
{
    HANDLE std = GetStdHandle(STD_OUTPUT_HANDLE);

    if (std && std != INVALID_HANDLE_VALUE && GetFileType(std) != FILE_TYPE_UNKNOWN)
        g_out = std; /* redirected to a file or pipe */
    else if (AttachConsole(ATTACH_PARENT_PROCESS))
        use_console(); /* started from a terminal */
}

/* printf for wide strings that also survives non-ASCII paths.
 * Use %ls for wide and %hs for narrow strings. */
static void say(const wchar_t *fmt, ...)
{
    wchar_t text[2048];
    char utf8[3 * 2048];
    DWORD mode, written;
    va_list args;
    int len;

    va_start(args, fmt);
    len = _vsnwprintf(text, ARRAYSIZE(text) - 1, fmt, args);
    va_end(args);
    if (len < 0)
        len = ARRAYSIZE(text) - 1;
    text[len] = L'\0';
    wcsncat(g_said, text, ARRAYSIZE(g_said) - wcslen(g_said) - 1);

    if (!g_out)
        return;
    if (GetConsoleMode(g_out, &mode)) {
        WriteConsoleW(g_out, text, len, &written, NULL);
    } else {
        len = WideCharToMultiByte(CP_UTF8, 0, text, len, utf8, sizeof utf8, NULL, NULL);
        WriteFile(g_out, utf8, len, &written, NULL);
    }
}

/* Puts what was said in a message box when nobody could have read it:
 * there is no console, or there is one that is about to disappear. */
static void show_if_unseen(UINT icon)
{
    if (!g_out || g_own_console)
        MessageBoxW(NULL, g_said, TITLE, icon | MB_SETFOREGROUND);
}

static int fail(void)
{
    show_if_unseen(MB_ICONERROR);
    return 1;
}

static const wchar_t *error_text(DWORD code)
{
    static wchar_t text[256];

    if (!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0,
                        text, ARRAYSIZE(text), NULL))
        text[0] = L'\0';
    return text;
}

/* --------------------------------------------------------------- addresses */

/* Fills list with this machine's IPv4 addresses, returns how many. */
static int local_addresses(struct local_address *list, int max)
{
    IP_ADAPTER_INFO *adapters, *adapter;
    IP_ADDR_STRING *ip;
    ULONG size = 0;
    int count = 0;

    if (GetAdaptersInfo(NULL, &size) != ERROR_BUFFER_OVERFLOW)
        return 0;
    adapters = HeapAlloc(GetProcessHeap(), 0, size);
    if (adapters && GetAdaptersInfo(adapters, &size) == ERROR_SUCCESS) {
        for (adapter = adapters; adapter; adapter = adapter->Next) {
            for (ip = &adapter->IpAddressList; ip && count < max; ip = ip->Next) {
                DWORD addr = inet_addr(ip->IpAddress.String);
                if (addr == INADDR_ANY || addr == INADDR_NONE) /* adapter without an address */
                    continue;
                list[count].addr = addr;
                lstrcpynA(list[count].ip, ip->IpAddress.String, sizeof list[count].ip);
                lstrcpynA(list[count].mask, ip->IpMask.String, sizeof list[count].mask);
                lstrcpynA(list[count].adapter, adapter->Description, sizeof list[count].adapter);
                count++;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, adapters);
    return count;
}

static void say_addresses(const struct local_address *list, int count)
{
    int i;

    for (i = 0; i < count; i++)
        say(L"  %-15hs  mask %-15hs  %hs\n", list[i].ip, list[i].mask, list[i].adapter);
}

/* Stores what was picked in the dialog. WritePrivateProfileString edits the
 * one line in place and leaves the rest of the ini, comments included, alone. */
static BOOL save_choice(const wchar_t *ini, const struct choice *choice, BOOL addr_changed)
{
    const BYTE *b = (const BYTE *)&choice->addr;
    wchar_t ip[16];
    BOOL ok = TRUE;

    /* An unchanged address is not rewritten, so a subnet-style setting stays. */
    if (addr_changed) {
        _snwprintf(ip, ARRAYSIZE(ip), L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        ok = WritePrivateProfileStringW(INI_SECTION, L"ip", ip, ini) && ok;
    }
    ok = WritePrivateProfileStringW(INI_SECTION, L"ask", choice->dont_ask ? L"0" : L"1", ini) && ok;
    ok = WritePrivateProfileStringW(INI_SECTION, L"log", choice->log ? L"1" : L"0", ini) && ok;
    return ok;
}

/* -------------------------------------------------------------- the game */

/* Returns the command line with the first argument (our own name) removed,
 * exactly as typed, so quoting of the game's arguments is preserved. */
static const wchar_t *skip_first_arg(const wchar_t *cmd)
{
    if (*cmd == L'"') {
        cmd++;
        while (*cmd && *cmd != L'"')
            cmd++;
        if (*cmd)
            cmd++;
    } else {
        while (*cmd && *cmd != L' ' && *cmd != L'\t')
            cmd++;
    }
    while (*cmd == L' ' || *cmd == L'\t')
        cmd++;
    return cmd;
}

/* The game needs administrator rights and we do not have them: start
 * ourselves again elevated (shows the UAC prompt) with the same arguments. */
static int relaunch_elevated(void)
{
    SHELLEXECUTEINFOW info = { sizeof info };
    wchar_t self[MAX_PATH], cwd[MAX_PATH];

    GetModuleFileNameW(NULL, self, ARRAYSIZE(self));
    info.lpVerb = L"runas";
    info.lpFile = self;
    info.lpParameters = skip_first_arg(GetCommandLineW());
    if (GetCurrentDirectoryW(ARRAYSIZE(cwd), cwd))
        info.lpDirectory = cwd; /* keeps relative paths in the arguments valid */
    info.nShow = SW_SHOWNORMAL;
    say(L"The game wants administrator rights, restarting portrebind elevated...\n");
    if (!ShellExecuteExW(&info)) {
        say(L"Could not elevate: %ls", error_text(GetLastError()));
        return fail();
    }
    return 0;
}

/* Starts the game suspended, injects the DLL, then lets the game run. */
static int launch(const wchar_t *exe, const wchar_t *cmdline, const wchar_t *dll)
{
    STARTUPINFOW startup = { sizeof startup };
    PROCESS_INFORMATION process;
    wchar_t dir[MAX_PATH], *file_part = NULL, *cmd;
    const wchar_t *cwd = NULL;
    const char *step;
    DWORD err;

    /* Run the game from its own directory, like a shortcut would. */
    if (GetFullPathNameW(exe, ARRAYSIZE(dir), dir, &file_part) && file_part &&
        GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES) {
        *file_part = L'\0';
        cwd = dir;
    }

    cmd = _wcsdup(cmdline); /* CreateProcessW wants a writable buffer */
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, cwd, &startup,
                        &process)) {
        err = GetLastError();
        if (err == ERROR_ELEVATION_REQUIRED)
            return relaunch_elevated();
        say(L"Could not start %ls\n  %ls", cmdline, error_text(err));
        return fail();
    }

    err = inject_dll(process.hProcess, dll, &step);
    if (err != ERROR_SUCCESS) {
        TerminateProcess(process.hProcess, 1);
        say(L"Started the game but could not inject into it, so it was stopped again.\n"
            L"  failed step: %hs\n  error %lu: %ls", step, err, error_text(err));
        return fail();
    }

    ResumeThread(process.hThread);
    say(L"Game started (pid %lu) with portrebind.dll inside.\n", process.dwProcessId);
    return 0;
}

static DWORD find_process(const wchar_t *name)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry = { sizeof entry };
    DWORD pid = 0;

    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (lstrcmpiW(entry.szExeFile, name) == 0)
                pid = entry.th32ProcessID;
        } while (!pid && Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

/* For when something else (Steam, EA app...) insists on starting the game. */
static int wait_and_inject(const wchar_t *name, const wchar_t *dll)
{
    const char *step;
    HANDLE process;
    DWORD pid, err;

    /* Waiting silently would look like nothing happened, so when there is no
     * terminal to print to, open a console window of our own. */
    if (!g_out && AllocConsole()) {
        use_console();
        g_own_console = TRUE;
    }
    say(L"Waiting for a process named %ls - start the game now.\n"
        L"Ctrl+C or closing this window gives up.\n", name);
    while (!(pid = find_process(name)))
        Sleep(250);

    process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                          PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        err = GetLastError();
        say(L"Found it (pid %lu) but could not open it: %ls", pid, error_text(err));
        if (err == ERROR_ACCESS_DENIED)
            say(L"The game probably runs as administrator; run portrebind as administrator too.\n");
        return fail();
    }

    err = inject_dll(process, dll, &step);
    if (err != ERROR_SUCCESS) {
        say(L"Found it (pid %lu) but could not inject.\n  failed step: %hs\n  error %lu: %ls", pid,
            step, err, error_text(err));
        return fail();
    }
    say(L"Injected portrebind.dll into %ls (pid %lu).\n", name, pid);
    return 0;
}

static void usage(void)
{
    say(L"portrebind - makes a game use the network interface you choose\n\n"
        L"  portrebind.exe                        start the game set in portrebind.ini\n"
        L"  portrebind.exe <game.exe> [args...]   start this game instead\n"
        L"  portrebind.exe --wait [name]          wait for a running process (default\n"
        L"                                        " DEFAULT_WAIT_NAME L") and inject into it\n"
        L"  portrebind.exe --list                 list this machine's IPv4 addresses\n\n"
        L"The address is picked in a dialog at start and kept in portrebind.ini.\n");
}

int main(void)
{
    wchar_t ini[MAX_PATH], dll[MAX_PATH], cmdline[MAX_PATH + 1100];
    struct local_address addresses[32];
    const wchar_t *exe, *cmd;
    struct config cfg;
    DWORD target;
    wchar_t **argv;
    int argc, count;

    output_init();
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return 1;
    count = local_addresses(addresses, ARRAYSIZE(addresses));

    if (argc >= 2 && (!wcscmp(argv[1], L"--help") || !wcscmp(argv[1], L"-h") ||
                      !wcscmp(argv[1], L"/?"))) {
        usage();
        show_if_unseen(MB_ICONINFORMATION);
        return 0;
    }
    if (argc >= 2 && !wcscmp(argv[1], L"--list")) {
        say_addresses(addresses, count);
        show_if_unseen(MB_ICONINFORMATION);
        return 0;
    }

    if (!path_next_to_module(NULL, INI_NAME, ini, ARRAYSIZE(ini)) ||
        !path_next_to_module(NULL, DLL_NAME, dll, ARRAYSIZE(dll))) {
        say(L"The path portrebind is installed in is too long.\n");
        return fail();
    }
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        say(L"Missing %ls - it has to sit next to portrebind.exe.\n", dll);
        return fail();
    }

    config_load(ini, &cfg);
    target = resolve_target_ip(cfg.ip, GetIpAddrTable);
    if (target && !ip_is_local(target, GetIpAddrTable))
        target = 0;

    /* Ask when told to, when the saved address is no good, or when Shift is
     * held down (the way back in after "don't show this again"). */
    if (count && (cfg.ask || !target || GetAsyncKeyState(VK_SHIFT) < 0)) {
        struct choice choice = { target, !cfg.ask, cfg.log };
        INT_PTR answer = ask_user(addresses, count, &choice);

        if (answer == IDCANCEL)
            return 0;
        if (answer == IDOK) {
            if (!save_choice(ini, &choice, choice.addr != target)) {
                say(L"Could not save the choice to\n  %ls\n%ls", ini, error_text(GetLastError()));
                return fail();
            }
            target = choice.addr;
        }
        /* Anything else: the dialog could not be shown. Go by the ini alone. */
    }

    /* The DLL reads the address from the ini as well, so complain here, where
     * the user can still see it, if that is not going to work. */
    if (!target) {
        if (!cfg.ip[0])
            say(L"No address configured. Set \"ip\" in\n  %ls\n", ini);
        else
            say(L"ip = %ls (from %ls)\ndoes not match any address of this machine.\n", cfg.ip, ini);
        say(L"\nAddresses available right now:\n");
        say_addresses(addresses, count);
        return fail();
    }
    say(L"Forcing the game onto %hs\n", inet_ntoa(*(struct in_addr *)&target));

    if (argc >= 2 && !wcscmp(argv[1], L"--wait"))
        return wait_and_inject(argc >= 3 ? argv[2] : DEFAULT_WAIT_NAME, dll);

    if (argc >= 2) {
        exe = argv[1];
        cmd = skip_first_arg(GetCommandLineW());
    } else if (cfg.game[0]) {
        exe = cfg.game;
        _snwprintf(cmdline, ARRAYSIZE(cmdline), L"\"%ls\" %ls", cfg.game, cfg.args);
        cmdline[ARRAYSIZE(cmdline) - 1] = L'\0';
        cmd = cmdline;
    } else {
        say(L"Nothing to start. Set \"game\" in\n  %ls\nor pass the game's exe as an argument.\n\n",
            ini);
        usage();
        return fail();
    }
    return launch(exe, cmd, dll);
}
