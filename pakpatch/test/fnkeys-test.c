/*
 * fnkeys-test.exe - the mod keys of fnmods.dll (fnkeys.c): the key names, and that a key another program holds as a
 * system-wide hotkey is found. Developer test, run by hand (test/fnkeys-test.ps1).
 *   fnkeys-test.exe self   F14 held by another thread of this process (as RegisterHotKey holds it for any program):
 *                          found; F13 free; F14 free again once that thread lets it go
 *   fnkeys-test.exe held   F14 held by another program (fnkeys-test.ps1 starts an AutoHotkey v2 script with F14::): found
 * Hotkeys need the signed-in user's window station: RegisterHotKey fails (1459) in an ssh session's. F13 and F14 are
 * keys no usual keyboard has, so holding them for a moment takes nothing from the user.
 * Build: zig cc -target x86_64-windows-gnu -O1 -o fnkeys-test.exe test/fnkeys-test.c -luser32
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../fnkeys.c"

static int fails;
static void expect(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

static HANDLE g_held, g_release;
static DWORD WINAPI holder(LPVOID r)
{
    int ok = RegisterHotKey(NULL, 1, 0, VK_F14);
    *(int *)r = ok ? 0 : (int)GetLastError();
    SetEvent(g_held);
    WaitForSingleObject(g_release, INFINITE);
    if (ok) UnregisterHotKey(NULL, 1);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "self";
    expect(key_vk("F8") == VK_F8 && key_vk("F10") == VK_F10 && key_vk("F24") == VK_F24, "key names F8 F10 F24");
    expect(key_vk("PageUp") == VK_PRIOR && key_vk("Home") == VK_HOME && key_vk("Insert") == VK_INSERT, "key names PageUp Home Insert");
    expect(key_vk("Tilde") == VK_OEM_3 && key_vk("K") == 'K', "key names Tilde K");
    expect(!key_vk("F0") && !key_vk("F25") && !key_vk("F100") && !key_vk("Space2") && !key_vk(""), "not key names: F0 F25 F100 Space2 empty");
    /* this session can hold hotkeys at all (not an ssh session's window station) */
    int can = RegisterHotKey(NULL, 2, 0, VK_F13);
    DWORD err = can ? 0 : GetLastError();
    if (can) UnregisterHotKey(NULL, 2);
    if (!can) printf("RegisterHotKey F13: error %lu\n", err);
    expect(can, "hotkeys can be registered in this session");
    if (!strcmp(mode, "self")) {
        int r = -1;
        g_held = CreateEventA(NULL, TRUE, FALSE, NULL); g_release = CreateEventA(NULL, TRUE, FALSE, NULL);
        HANDLE t = CreateThread(NULL, 0, holder, &r, 0, NULL);
        WaitForSingleObject(g_held, INFINITE);
        expect(r == 0, "the other thread holds F14");
        expect(key_taken(VK_F14) == 1, "F14 held elsewhere: found");
        expect(key_taken(VK_F13) == 0, "F13 free");
        SetEvent(g_release); WaitForSingleObject(t, INFINITE); CloseHandle(t);
        expect(key_taken(VK_F14) == 0, "F14 free once let go");
    } else {
        expect(key_taken(VK_F14) == 1, "F14 held by the AutoHotkey script: found");
        expect(key_taken(VK_F13) == 0, "F13 free");
    }
    /* the check leaves nothing registered: this thread can take F13 itself and give it back */
    int mine = RegisterHotKey(NULL, 3, 0, VK_F13);
    if (mine) UnregisterHotKey(NULL, 3);
    expect(mine, "F13 can be registered after the checks");
    printf("%s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
