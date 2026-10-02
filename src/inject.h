#ifndef PORTREBIND_INJECT_H
#define PORTREBIND_INJECT_H

#include <windows.h>

/* Makes the given process load dll_path. Needs a handle that allows creating
 * threads and writing memory (what CreateProcess returns is enough).
 * Returns ERROR_SUCCESS, or a Win32 error code with *failed_step naming the
 * step that broke. */
DWORD inject_dll(HANDLE process, const wchar_t *dll_path, const char **failed_step);

#endif
