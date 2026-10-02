#ifndef PORTREBIND_DIALOG_H
#define PORTREBIND_DIALOG_H

#include "common.h"

/* One IPv4 address of this machine. */
struct local_address {
    DWORD addr;
    char ip[16];
    char mask[16];
    char adapter[MAX_ADAPTER_DESCRIPTION_LENGTH + 4];
};

/* What the dialog shows when it opens and what the user left it at. */
struct choice {
    DWORD addr;
    BOOL dont_ask;
    BOOL log;
};

/* Shows the "pick an address" dialog. Returns IDOK (choice was filled in),
 * IDCANCEL, or -1 if the dialog could not be shown at all. */
INT_PTR ask_user(const struct local_address *list, int count, struct choice *choice);

#endif
