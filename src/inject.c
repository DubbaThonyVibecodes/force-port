/*
 * Classic LoadLibrary injection:
 *   1. write the DLL path into the target's memory,
 *   2. start a thread in the target whose entry point is LoadLibraryW and
 *      whose argument is that path.
 *
 * This works because kernel32.dll sits at the same address in every process
 * of the same bitness, so our LoadLibraryW pointer is valid over there too.
 * It also works on a process created suspended that has not run yet: the new
 * thread makes the Windows loader finish setting the process up first.
 */
#include "inject.h"

/* A 32-bit injector can only inject into 32-bit processes (and 64 into 64). */
static BOOL same_bitness(HANDLE process)
{
    BOOL self_wow64 = FALSE, target_wow64 = FALSE;

    if (!IsWow64Process(GetCurrentProcess(), &self_wow64) ||
        !IsWow64Process(process, &target_wow64))
        return TRUE; /* cannot tell; let the injection itself fail */
    return self_wow64 == target_wow64;
}

DWORD inject_dll(HANDLE process, const wchar_t *dll_path, const char **failed_step)
{
    SIZE_T path_bytes = (wcslen(dll_path) + 1) * sizeof(wchar_t);
    void *load_library = (void *)GetProcAddress(GetModuleHandleW(L"kernel32"), "LoadLibraryW");
    void *remote_path = NULL;
    HANDLE thread = NULL;
    DWORD loaded = 0, err = ERROR_SUCCESS;

    *failed_step = "bitness check (target is not a 32-bit process)";
    if (!same_bitness(process)) {
        err = ERROR_BAD_EXE_FORMAT;
        goto out;
    }

    *failed_step = "VirtualAllocEx";
    remote_path = VirtualAllocEx(process, NULL, path_bytes, MEM_COMMIT | MEM_RESERVE,
                                 PAGE_READWRITE);
    if (!remote_path)
        goto fail;

    *failed_step = "WriteProcessMemory";
    if (!WriteProcessMemory(process, remote_path, dll_path, path_bytes, NULL))
        goto fail;

    *failed_step = "CreateRemoteThread";
    thread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)load_library,
                                remote_path, 0, NULL);
    if (!thread)
        goto fail;

    *failed_step = "waiting for LoadLibraryW in the target";
    if (WaitForSingleObject(thread, 30000) != WAIT_OBJECT_0) {
        err = ERROR_TIMEOUT;
        goto out;
    }

    /* The thread's exit code is LoadLibraryW's return value: NULL = failed. */
    *failed_step = "LoadLibraryW in the target (is the DLL readable by the game?)";
    if (!GetExitCodeThread(thread, &loaded))
        goto fail;
    if (!loaded)
        err = ERROR_MOD_NOT_FOUND;
    goto out;

fail:
    err = GetLastError();
out:
    if (thread)
        CloseHandle(thread);
    if (remote_path)
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    return err;
}
