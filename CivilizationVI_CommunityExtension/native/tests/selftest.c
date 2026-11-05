/* Isolated test driver only; not part of CE or its build system.
 * Initialization/MinHook/shim entry must never run during these synthetic tests.
 * Abort-if-called stubs make that scope executable rather than assumed. */
#include <stdio.h>
#include <stdlib.h>
#include "MinHook.h"
int CE_AIDistrictScoresSelfTest(void);
static MH_STATUS unexpected(const char *name) {
    fprintf(stderr, "Unexpected initialization/shim call: %s\n", name);
    abort();
}
void bridge_net_entry(void) { (void)unexpected("bridge_net_entry"); }
MH_STATUS WINAPI MH_CreateHook(LPVOID target, LPVOID detour, LPVOID *out) {
    (void)target; (void)detour; (void)out; return unexpected("MH_CreateHook");
}
MH_STATUS WINAPI MH_QueueEnableHook(LPVOID target) {
    (void)target; return unexpected("MH_QueueEnableHook");
}
MH_STATUS WINAPI MH_ApplyQueued(void) { return unexpected("MH_ApplyQueued"); }
MH_STATUS WINAPI MH_DisableHook(LPVOID target) {
    (void)target; return unexpected("MH_DisableHook");
}
MH_STATUS WINAPI MH_RemoveHook(LPVOID target) {
    (void)target; return unexpected("MH_RemoveHook");
}
int main(void) {
    int count = CE_AIDistrictScoresSelfTest();
    printf("CE_AIDistrictScoresSelfTest=%d\n", count);
    return count == 36 ? 0 : 1;
}
