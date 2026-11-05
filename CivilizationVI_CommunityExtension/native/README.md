# Experimental district-site score API

This module adds generic district/site incentives to CE. A consuming mod publishes
numeric player/city/district/plot adjustments through gameplay Lua. CE applies
qualified adjustments to fresh native outputs and records diagnostic observations.
It contains no mod-specific adjacency rules, construction orders or subsidies.

This is a source port to `develop`; the earlier master-based prototype's runtime
results do **not** qualify this develop integration. Build, native lifecycle and
mod compatibility must be checked again before a release or upstream proposal.

## Reviewer walkthrough

Read the integration in this order:

1. `Main.cpp` calls Initialize once, registers bindings for gameplay states and
   resets policy at context boundaries. Upstream develop's lifecycle remains in
   charge; this module never creates a second loader or Lua VM.
2. `install_hooks` qualifies the original image and public HKS exports before
   installing hooks. `hook_entry.asm` preserves the NetValue caller's R12 row
   and original seven arguments, with Win64 stack/unwind metadata.
3. `l_begin` / `l_allow` / `l_entry` construct staging. `l_commit` is the only
   publication point; `bridge_net` copies one admitted entry before calling the
   original routine and checks its generation again before writing the output.
4. The three `hook_*` monitors call originals and journal numeric observations.
   `l_status` formats under its shared lock; `l_trace_read` copies a bounded page
   and formats after releasing the lock. Both push HKS values outside the lock.
5. `reset_policy` retires future callbacks without claiming to roll back native
   caches/requests. The isolated self-test exercises synthetic data, not worker
   scheduling or the current develop integration.

The RVAs and layouts were recovered from the pinned Windows XP2 disassembly and
corroborated by the earlier four-hook prototype. They are not cross-platform CE
struct definitions. Relevant native contracts are:

| RVA | Contract / qualification |
|---|---|
| `52a440` | NetValue returns the caller's int32 output pointer; other arguments pass through unchanged. |
| `3e30be` | Ordinary caller return address; R12 identifies its plot-analysis row. Other NetValue callers are not adjusted. |
| `3dfee0` | CalculateDistrictValues(this) returns void; observe completed cache only. |
| `3e4a50` | GetBestDistrict(this, int32 xy[2]) returns district index, or -1 with no valid coordinates. |
| `3e7f40` | PresentValueOf(this, int32 *out, district, optional plot) returns out; only explicit-plot calls are traced. |
| `5bdac0` | RegisterScriptData prefix is verified before CE modifies it; this module does not detour it. |

`observe_enter`/`observe_cache` document the associated CityBuild, City and row
offsets. Hash, prefix and span checks are necessary bounds, not a substitute for
native object lifetime. A changed GameCore requires requalifying ABI, call-site
register use and every layout offset, rather than merely changing a hash.

## Existing project integration

Use CE's existing Visual Studio solution and dependency setup. The added source
is compiled as C11 by v143, the Win64 shim uses MSVC's MASM customization, and
both x64 configurations link the Windows SDK `Bcrypt.lib`. The module reuses CE's
existing MinHook dependency. No additional build system or player runtime is used.

Initialize after CE's HavokScript/MinHook setup and before its registration hooks
alter the qualified image. Register bindings in each gameplay Lua state. Reset
policy before creating and destroying a game context; hooks remain process-owned.
The existing develop `std::call_once`, logger, export layout and destroy forwarder
are retained. Failure disables this capability, rather than silently providing
an unqualified score consumer.

The opaque Lua-state interface deliberately resolves the required public HKS
exports independently. The JSON helper uses bounded 240-byte literal fragments,
`luaL_checkstack` and incremental `hksi_lua_concat`, avoiding the shipped
formatter's 511-byte payload limit. No worker hook invokes HKS or gameplay APIs.

## Qualified native target

Windows x64 Gathering Storm `GameCore_XP2_FinalRelease.dll` only:

```
SHA-256 324c51e9ea3531758842e16c69e6cddbefbb226c5b675c3d6e60111646c2e98c
PE timestamp 1719431003
Image size 12976128
```

Initialization checks the original file hash, loaded metadata, all four hook
prefixes and the registration prefix. `target_identity.h` contains qualification
metadata and prefixes. No GameCore DLL is bundled. Other builds/platforms are
outside this contract.

## Gameplay Lua interface

All globals start with `CE_AIDistrictScores`. Mutating methods return 1 on
success, 0 on rejection; consumers must catch argument errors and Disable after
any partial/rejected publication.

| Suffix | Arguments | Meaning |
|---|---|---|
| `Begin` | version, width, height, districtCount | Disable old policy and open empty staging. |
| `Allow` | playerId | Admit one original AI identity. |
| `Entry` | playerId, cityId, districtIndex, plotIndex, deltaQ8 | Add a distinct pair adjustment. |
| `Commit` | none | Publish the complete staged table atomically. |
| `Disable` | none | Retire policy/staging and advance generation. |
| `Status` | none | JSON readiness, counters, samples and trace metadata. |
| `TraceCount` | none | Retained native journal length. |
| `TraceRead` | offset, limit, expectedGeneration | Non-consuming JSON journal page. |

Argument conversion occurs before any lock: HKS may longjmp on a bad argument.
Consumers must catch that error and call Disable. Once conversion succeeds,
Begin retires the current policy before range/version validation. A rejected
Allow or Entry poisons staging; Commit rejects a partial batch or any native
fault. Only context reset clears faults. Disable retains the published version
and journal; Commit/reset clear the journal. This distinction matters when
harvesting evidence or replacing a rejected publication.

Versions increase within a context. Width/height are 1-256; districtCount is
1-4096 and covers the highest native index plus one. Player IDs are 0-63, city IDs
nonnegative, districts 1 through districtCount-1, plots zero-based y*width+x.
At most 4096 unique entries are admitted. Capture original human/AI roles before
autoplay changes them; the native allowlist does not discover original humans.

Deltas are signed Q8 utility units (256 Q8 = 1 utility), not yields or build orders.
Bounds are -262144 through +1048576, excluding zero; at most 4096 native changes
are admitted per publication. Zero and scores at/below -2559744 are protected.
Deductions clamp at -1048576 without raising a base already at/below that floor.
Unsafe arithmetic, including results above INT_MAX/2, is rejected unchanged.
These inherited diagnostic bounds are not recommended tuning weights.

## Diagnostics and lifetime

The NetValue hook preserves native arguments and returned output identity. Three
typed read-only hooks record final-cache, GetBestDistrict and plot-specific
PresentValue observations. No policy runs in these callbacks. Cache/PV records
cover published pairs; a best record can show another district in a scoped city.

Status ABI1 retains the first 64 changed samples and exposes trace_count,
trace_capacity (16384) and active. Trace kinds are net/cache/best/pv. Each record
contains generation/version, player/city/district/plot, base/result, reason and
eligibility. Net reasons: 0 adjusted, 2 protected, 3 arithmetic rejected,
4 write budget exhausted. A best district/plot of -1 reports no winner.

TraceRead offsets are zero-based; limits are 1-64. JSON includes generation,
version, total, offset, next, stale and records. A generation mismatch returns an
empty stale page. Disable before reading for a stable journal, then use the new
Status generation; records may retain an older capture generation. Save pages
before Commit/reset, which clear the journal. Capacity exhaustion disables
incentives and reports a fault rather than silently losing evidence.

Storage and lookup deliberately favor an inspectable bounded implementation:
fixed arrays, linear pair lookup and no worker-side heap allocation. The API has
one global publisher per context, with no per-mod ownership token; multiple
consuming mods must coordinate. Large-world performance and a composable
multi-consumer API remain review topics rather than implicit guarantees.

The SRW lock and generation fence prevent older admitted calls from writing or
appending after replacement/reset. This does not make an entire analysis pass
atomic, cancel old requests or remove cached influence. Policy is not serialized;
a consuming mod must reconstruct it after loading a save. The carrier is pinned before hooks become active and remains resident until
process exit. Live FreeLibrary unload is intentionally unsupported; there is no
hook-removal/drain protocol. Multiplayer and general unload safety are unqualified.

`CE_AI_DISTRICT_SCORES_TEST` enables an isolated 36-check native self-test entry,
`CE_AIDistrictScoresSelfTest`, absent from normal builds. It uses synthetic
buffers/stub originals and exercises arithmetic, all four hook families, journal
paging, reset and bounded HKS string transport without loading Civ. Passing the
old build's tests is not a substitute for compiling/running this port.

The earlier prototype's evidence is preserved in the [qualification report](https://hennogous.github.io/Henno-Mods/ai/sources/project/docs/research/mods/csc-ce-policy-parity-20261104).
It explains the recovered signatures and actual prior runtime observations; it
is not a test receipt for this develop port. Supported MSVC/full-DLL and fresh
native-game qualification remain pending for this source revision.
