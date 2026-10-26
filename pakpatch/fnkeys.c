/*
 * fnkeys.c - the mod keys of fnmods.dll (#included by fnmods.c, and by test/fnkeys-test.c): the key names mods.json uses
 * and the check for a key that another program holds as a system-wide hotkey. Such a key (RegisterHotKey: an AutoHotkey
 * script's plain "F8::", ...) is handed to that program by Windows: no window, the game's included, gets its
 * WM_KEYDOWN, so neither the menu (fnmenu.c) nor the engine's console sees it.
 */

/* the Windows virtual key of an engine key name of mods.json (F1-F24, Insert, Delete, Home, End, PageUp, PageDown, Tilde,
   a letter or a digit); 0 = none of those */
static UINT key_vk(const char *n)
{
    if ((n[0] == 'F' || n[0] == 'f') && n[1] >= '1' && n[1] <= '9' && (!n[2] || (n[2] >= '0' && n[2] <= '9' && !n[3]))) {
        int k = atoi(n + 1);
        return k <= 24 ? (UINT)(VK_F1 + k - 1) : 0;
    }
    static const struct { const char *n; UINT vk; } K[] = {
        { "Insert", VK_INSERT }, { "Delete", VK_DELETE }, { "Home", VK_HOME }, { "End", VK_END }, { "PageUp", VK_PRIOR }, { "PageDown", VK_NEXT },
        { "Tilde", VK_OEM_3 },
    };
    for (int i = 0; i < (int)(sizeof K / sizeof K[0]); i++) if (!_stricmp(n, K[i].n)) return K[i].vk;
    if (n[0] && !n[1] && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9'))) return (UINT)n[0];
    return 0;
}
/* 1 = another program holds vk (no modifiers) as a system-wide hotkey: registering it here for a moment fails with
   ERROR_HOTKEY_ALREADY_REGISTERED. Registered and released on this thread at once; tried twice 300 ms apart, so the
   launcher's own check of the same key at that moment is not taken for another program. */
static int key_taken(UINT vk)
{
    for (int t = 0; t < 2; t++) {
        if (RegisterHotKey(NULL, 0x7F08, 0, vk)) { UnregisterHotKey(NULL, 0x7F08); return 0; }
        if (GetLastError() != ERROR_HOTKEY_ALREADY_REGISTERED) return 0;
        if (!t) Sleep(300);
    }
    return 1;
}
