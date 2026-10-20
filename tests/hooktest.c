/* Test harness for fnredirect.dll: a fake curl_easy_setopt with the same machine code prologue as the one in
   Fortnite 17.50 (sub rsp,28h / null check / return 43 / call Curl_vsetopt). It records what it receives. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
static char last_url[512]; static long long last_val = -1; static int last_opt;
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
int main(int argc, char **argv) {
  SetEnvironmentVariableA("FNOFFLINE_PORT", "3551");
  SetEnvironmentVariableA("FNOFFLINE_LOG", argc > 2 ? argv[2] : "hooktest.log");
  unsigned char *p = (unsigned char *)fake_setopt; printf("prologue:"); for (int i = 0; i < 26; i++) printf(" %02X", p[i]); printf("\n");
  if (!strcmp(argv[1], "wait")) Sleep(4000); /* injected from outside (launcher self-test) */
  else if (!LoadLibraryA(argv[1])) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
  int fails = 0;
  struct { const char *in, *want; } t[] = {
    { "https://account-public-service-prod.ol.epicgames.com/account/api/oauth/token", "http://127.0.0.1:3551/account/api/oauth/token" },
    { "https://fortnite-public-service-prod11.ol.epicgames.com/fortnite/api/game/v2/profile/x/client/QueryProfile?profileId=athena&rvn=-1", "http://127.0.0.1:3551/fortnite/api/game/v2/profile/x/client/QueryProfile?profileId=athena&rvn=-1" },
    { "https://datarouter.ol.epicgames.com:443/datarouter/api/v1/public/data", "http://127.0.0.1:3551/datarouter/api/v1/public/data" },
    { "https://example.com", "http://127.0.0.1:3551/" },
    { "http://127.0.0.1:3551/offline/health", "http://127.0.0.1:3551/offline/health" },
  };
  for (int i = 0; i < 5; i++) { last_url[0] = 0; fake_setopt((void *)1, 10002, t[i].in); int ok = !strcmp(last_url, t[i].want); fails += !ok; printf("%s %s -> %s\n", ok ? "OK  " : "FAIL", t[i].in, last_url); }
  fake_setopt((void *)1, 64, (void *)1); printf("%s VERIFYPEER 1 -> %lld\n", last_val == 0 ? "OK  " : "FAIL", last_val); fails += last_val != 0;
  last_val = -1; fake_setopt((void *)1, 81, (void *)2); printf("%s VERIFYHOST 2 -> %lld\n", last_val == 0 ? "OK  " : "FAIL", last_val); fails += last_val != 0;
  last_val = -1; fake_setopt((void *)1, 13, (void *)30); printf("%s TIMEOUT 30 -> %lld\n", last_val == 30 ? "OK  " : "FAIL", last_val); fails += last_val != 30;
  printf("null handle -> %d (want 43)\n", fake_setopt(0, 10002, "x"));
  printf(fails ? "FAILED %d\n" : "ALL OK\n", fails); return fails;
}
