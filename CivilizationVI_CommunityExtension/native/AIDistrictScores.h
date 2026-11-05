/* Experimental CE-owned district-score API, compiled into the CE carrier.
 * See native/README.md for the Lua publication contract and target qualification.
 * No consumer should call these lifecycle functions as a dynamic addon loader. */
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif

/* CE process-once initialization: core is CE Runtime's borrowed original module.
 * Call after MinHook/HKS initialization but before CE hooks modify registration.
 * Returns 1 only when the exact target/transport and all four hooks are ready.
 * Failure leaves this capability unavailable; other CE features retain their
 * normal initialization. This is not a concurrent reinitialization interface. */
int CE_AIDistrictScoresInitialize(HMODULE core);

/* Called by CE's normal gameplay-state registration, on that state's Lua thread.
 * L remains engine-owned; native workers never retain or invoke this Lua state.
 * Bindings are published only when the required HKS exports have resolved. */
void CE_AIDistrictScoresRegisterLua(void *L);

/* Call before creating and before destroying a game context. Retire admission,
 * clear policy/version/journal/fault state and advance the generation fence.
 * Hooks and cumulative invocation accounting remain process-owned. This does
 * not drain native workers or undo cached/request influence in engine memory. */
void CE_AIDistrictScoresReset(void);
#ifdef __cplusplus
}
#endif
