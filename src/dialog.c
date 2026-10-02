/* The "pick an address" dialog. Its layout lives in launcher.rc. */
#include "dialog.h"
#include "resource.h"
#include <commctrl.h>
#include <stdio.h>

struct dialog_state {
    const struct local_address *list;
    int count;
    struct choice *choice;
};

static void fill(HWND dlg, const struct dialog_state *state)
{
    char line[200];
    int i, selected = 0;

    for (i = 0; i < state->count; i++) {
        snprintf(line, sizeof line, "%s  -  %s", state->list[i].ip, state->list[i].adapter);
        SendDlgItemMessageA(dlg, IDC_IP, CB_ADDSTRING, 0, (LPARAM)line);
        if (state->list[i].addr == state->choice->addr)
            selected = i;
    }
    SendDlgItemMessageW(dlg, IDC_IP, CB_SETCURSEL, selected, 0);
    CheckDlgButton(dlg, IDC_DONT_ASK, state->choice->dont_ask ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_LOG, state->choice->log ? BST_CHECKED : BST_UNCHECKED);
}

static void read_back(HWND dlg, const struct dialog_state *state)
{
    int selected = (int)SendDlgItemMessageW(dlg, IDC_IP, CB_GETCURSEL, 0, 0);

    if (selected >= 0 && selected < state->count)
        state->choice->addr = state->list[selected].addr;
    state->choice->dont_ask = IsDlgButtonChecked(dlg, IDC_DONT_ASK) == BST_CHECKED;
    state->choice->log = IsDlgButtonChecked(dlg, IDC_LOG) == BST_CHECKED;
}

static INT_PTR CALLBACK dialog_proc(HWND dlg, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_INITDIALOG:
        /* lparam is the dialog_state from ask_user(); keep it for later. */
        SetWindowLongPtrW(dlg, DWLP_USER, lparam);
        fill(dlg, (const struct dialog_state *)lparam);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK)
            read_back(dlg, (const struct dialog_state *)GetWindowLongPtrW(dlg, DWLP_USER));
        if (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL)
            EndDialog(dlg, LOWORD(wparam));
        return TRUE;
    }
    return FALSE;
}

INT_PTR ask_user(const struct local_address *list, int count, struct choice *choice)
{
    struct dialog_state state = { list, count, choice };

    InitCommonControls(); /* loads the themed controls the manifest asks for */
    return DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_PICK), NULL, dialog_proc,
                           (LPARAM)&state);
}
