/* Test harness for fnredirect.dll: a fake curl_easy_setopt with the same machine code prologue as the one in
   Fortnite 17.50 (sub rsp,28h / null check / return 43 / call Curl_vsetopt). It records what it receives. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
static __thread char last_url[512]; static long long last_val = -1; static int last_opt;
int vsetopt(void *h, int opt, void **va) { last_opt = opt; if (opt == 10002) strncpy(last_url, (const char *)va[0], 511); else last_val = (long long)va[0]; return 0; }
__asm__(
".globl fake_setopt\n"
"fake_setopt:\n"
"  movl %edx, 0x10(%rsp)\n"            /* 89 54 24 10 */
"  movq %r8, 0x18(%rsp)\n"             /* 4C 89 44 24 18 */
"  movq %r9, 0x20(%rsp)\n"             /* 4C 89 4C 24 20 */
"  subq $0x28, %rsp\n"                 /* 48 83 EC 28 */
"  testq %rcx, %rcx\n"                 /* 48 85 C9 */
"  jne 1f\n"                           /* 75 08 */
"  leal 0x2b(%rcx), %eax\n"            /* 8D 41 2B */
"  addq $0x28, %rsp\n"
"  ret\n"
"1: leaq 0x40(%rsp), %r8\n"
"  call vsetopt\n"
"  addq $0x28, %rsp\n"
"  ret\n");
int fake_setopt(void *h, int opt, ...);

/* hwbp mode (FNOFFLINE_CURL_HOOK=hwbp): threads that exist BEFORE the DLL loads are armed only by its thread
   enumeration (arm_others: NtQuerySystemInformation since 19-oct-2026, Toolhelp before), threads created after it
   by DLL_THREAD_ATTACH. Each one calls the fake curl_easy_setopt and must see the URL redirected. */
static HANDLE go; static volatile LONG oks, runs;
static DWORD WINAPI worker(LPVOID p) {
  WaitForSingleObject(go, INFINITE);
  last_url[0] = 0;
  for (int i = 0; i < 50; i++) { fake_setopt((void *)1, 10002, "https://account-public-service-prod.ol.epicgames.com/account/api/oauth/token"); }
  if (!strcmp(last_url, "http://127.0.0.1:3551/account/api/oauth/token")) InterlockedIncrement(&oks);
  InterlockedIncrement(&runs); return 0;
}
int main(int argc, char **argv) {
  SetEnvironmentVariableA("FNOFFLINE_PORT", "3551");
  SetEnvironmentVariableA("FNOFFLINE_LOG", argc > 2 ? argv[2] : "hwbptest.log");
  SetEnvironmentVariableA("FNOFFLINE_CURL_HOOK", "hwbp");
  go = CreateEventA(NULL, TRUE, FALSE, NULL);
  enum { BEFORE = 24, AFTER = 8 };
  HANDLE th[BEFORE + AFTER];
  for (int i = 0; i < BEFORE; i++) th[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
  if (!LoadLibraryA(argv[1])) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
  Sleep(1500); /* the first pass runs in DllMain/hook install; the rearm thread repeats every 100 ms */
  for (int i = BEFORE; i < BEFORE + AFTER; i++) th[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
  Sleep(300);
  SetEvent(go);
  WaitForMultipleObjects(BEFORE + AFTER, th, TRUE, 20000);
  /* last_url is shared: check per thread through the counter instead */
  printf("threads %ld/%d redirected (%d created before the DLL, %d after)\n", oks, BEFORE + AFTER, BEFORE, AFTER);
  int fails = oks != BEFORE + AFTER;
  for (int i = 0; i < 200; i++) Sleep(10); /* let the rearm thread run a few more passes: it must not crash */
  printf(fails ? "FAILED\n" : "ALL OK\n"); return fails;
}
