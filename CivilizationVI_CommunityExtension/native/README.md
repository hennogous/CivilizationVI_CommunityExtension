# Experimental district-site score API

This module adds generic district/site incentives to CE. A consuming mod publishes
numeric player/city/district/plot adjustments through gameplay Lua. CE applies
qualified adjustments to fresh native outputs and records diagnostic observations.
It contains no mod-specific adjacency rules, construction orders or subsidies.

This development branch follows CE's C++ module, Runtime hook and HavokScript
binding conventions. It adds an optional late write to the earlier score API.
The prior build's qualification is retained separately below. Upstream review,
broader compatibility and release remain separate.

## Reviewer walkthrough

Read the integration in this order:

1. `Main.cpp` calls `AI::DistrictScores::Create` once, registers the gameplay
   Lua bindings and retires the policy at context boundaries.
2. `AIDistrictScores.cpp` qualifies the original image and delegates grouped
   hook installation to `Runtime::InstallHookGroup`. Runtime creates all
   trampolines before enabling any hook and rolls back this group's targets
   on failure. `hook_entry.asm` preserves the NetValue caller's R12 row and
   original seven arguments, with Win64 stack and unwind metadata.
3. Lua stages a table using Begin, Allow and Entry or LateEntry. Commit
   publishes it under the policy lock. The early hook preserves the original
   NetValue result pointer. The late hook checks the cache around aggregation.
   Both recheck the publication generation after the original routine.
4. The bounded journal records write attempts and the cache, winner and
   plot-specific present value. Hooks never call HKS. Lua functions release
   lock guards before invoking HKS, which can exit via longjmp.

The RVAs and layouts were recovered from the pinned Windows XP2 disassembly and
corroborated by the earlier four-hook prototype. They are not cross-platform CE
struct definitions. Relevant native contracts are:

| RVA | Contract / qualification |
|---|---|
| `52a440` | NetValue returns the caller's int32 output pointer; other arguments pass through unchanged. |
| `3e30be` | Ordinary caller return address; R12 identifies its plot-analysis row. Other NetValue callers are not adjusted. |
| `3dfee0` | CalculateDistrictValues(this) returns void. Only the qualified caller may apply late entries. |
| `3e293c` / `3e2941` | DoTurnActivate's call to aggregation and its return. Verify the call target and observe completion before district selection. |
| `3e4a50` | GetBestDistrict(this, int32 xy[2]) returns district index, or -1 with no valid coordinates. |
| `3e7f40` | PresentValueOf(this, int32 *out, district, optional plot) returns out; only explicit-plot calls are traced. |
| `5bdac0` | RegisterScriptData prefix is verified before CE modifies it; this module does not detour it. |

Named function addresses and field offsets live in `target_identity.h`. The
late matrix and row alias were verified in the
[late placement replay](https://hennogous.github.io/Henno-Mods/ai/sources/project/docs/research/mods/csc-ai-late-placement-20261105).
A changed GameCore requires requalifying ABI, call-site register use and layout.
The game owns every pointer. Bounds checks alone cannot guarantee lifetime.

## Optional ordinary building nomination

The pinned Windows XP2 selector compares an effective Q8 building score after
its ordinary prerequisite checks. At RVA `3e49d5`, CE replaces a verified
five-byte flag restore with a CALL through a near relay. The MASM entry preserves
volatile registers, replays the flag restore and returns to the untouched
comparison at `3e49da`. The frame supplies the original caller return and
flags. Only return `3e160a` with flags 0/0 may receive a bid.

Runtime owns the relay and original bytes. Unsupported bytes, unavailable
near allocation or a failed patch leave this capability unavailable. Runtime
pins the target module for the process before replacing the instruction, so
failed restoration cannot strand a call into unloaded code. Context reset
retires the published policy. The adapter changes one effective score only.
Cached values, accepted offers
and production arbitration keep their native values.

`CE_AIBuildingNominationBid(player, city, building, deltaQ8)` stages a positive
preference inside the existing Begin/Allow/Commit publication. The cap is
512 score units (131,072 Q8). The strongest duplicate wins. Nonpositive base
scores and arithmetic overflow are preserved. `CE_AIBuildingNominationStatus()`
reports readiness, the cap and the first applied bid in a publication.
The data and economic policy belong to the calling mod.

## Existing project integration

Use CE's existing Visual Studio solution and dependency setup. The feature
builds with CE's C++20 settings. The Win64 shim uses the MASM customization, and
both x64 configurations link the Windows SDK `Bcrypt.lib`.

Initialize after CE's HavokScript/MinHook setup and before its registration hooks
alter the qualified image. Register bindings in each gameplay Lua state. Reset
policy before creating and destroying a game context; hooks remain process-owned.
The existing develop `std::call_once`, logger, export layout and destroy forwarder
are retained. Failure disables this capability, rather than silently providing
an unqualified score consumer.

The feature uses CE's typed HavokScript imports and `PushLuaMethod`. The JSON
helper uses bounded 240-byte literal fragments, `luaL_checkstack` and incremental
`hksi_lua_concat` to stay within the shipped formatter's 511-byte limit.

## Qualified native target

Windows x64 Gathering Storm `GameCore_XP2_FinalRelease.dll`, Steam build
26407122. The November hotfix retains the earlier XP2 instruction bytes,
section layout and AI caller sites. This topic branch is an offline port
candidate pending qualification on a compatible upstream CE base.

```
SHA-256 092abfbb2176f8df04b5212f819dc269dd83aea866ff05aaf489904a1ff7c41f
PE timestamp 1793649108
Image size 12976128
```

Initialization checks the original file hash, loaded metadata, all four hook
prefixes, the registration prefix and the aggregation caller's relative CALL. `target_identity.h` contains qualification
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
| `Entry` | playerId, cityId, districtIndex, plotIndex, deltaQ8 | Add an early NetValue adjustment. |
| `LateEntry` | playerId, cityId, districtIndex, plotIndex, expectedBeforeQ8, expectedFinalQ8, deltaQ8 | Add an adjustment after building aggregation. |
| `Commit` | none | Publish the complete staged table atomically. |
| `Disable` | none | Retire policy/staging and advance generation. |
| `Status` | none | JSON readiness, counters, samples and trace metadata. |
| `TraceCount` | none | Retained native journal length. |
| `TraceRead` | offset, limit, expectedGeneration | Non-consuming JSON journal page. |

Argument conversion occurs before any lock: HKS may longjmp on a bad argument.
Consumers must catch that error and call Disable. Once conversion succeeds,
Begin retires the current policy before range/version validation. A rejected
Allow, Entry or LateEntry poisons staging; Commit rejects a partial batch or any native
fault. Only context reset clears faults. Disable retains the published version
and journal; Commit/reset clear the journal. This distinction matters when
harvesting evidence or replacing a rejected publication.

Versions increase within a context. Width/height are 1-256; districtCount is
1-4096 and covers the highest native index plus one. Player IDs are 0-63, city IDs
nonnegative, districts 1 through districtCount-1, plots zero-based y*width+x.
At most 4096 unique pairs are admitted across both entry methods. Capture original human/AI roles before
autoplay changes them; the native allowlist does not discover original humans.

Deltas are signed Q8 utility units (256 Q8 = 1 utility), not yields or build orders.
Bounds are -262144 through +1048576, excluding zero; at most 4096 native changes
are admitted per publication. Zero and scores at/below -2559744 are protected.
Deductions clamp at -1048576 without raising a base already at/below that floor.
Unsafe arithmetic, including results above INT_MAX/2, is rejected unchanged.
These inherited diagnostic bounds are not recommended tuning weights.

### Late score contract

LateEntry uses the same Begin, Allow and Commit transaction and write budget.
The expected scores are the pair's positive values at aggregation entry and
return, before the CE delta. The final native value must be at least the entry
value, and the adjusted result must remain positive and within the existing
numeric ceiling. A pair can appear once, through either Entry or LateEntry.

The late hook admits only a non-center, owned, district-eligible, unreserved row
whose row scores alias the writable matrix. It checks the expected entry value
before native aggregation, then rechecks row identity, reservation, matrix,
finished value and publication generation before the write. A mismatched
calculation keeps its native value. A new native calculation may independently
qualify again. CE leaves the building forecast and native candidate eligibility
alone. The consumer supplies the expected values and a delta justified by its
own evidence. This API adds no building-component query or Lua worker callback.

## Diagnostics and lifetime

The NetValue hook preserves native arguments and returned output identity.
The aggregation hook records the cache after any late write. The winner and
plot-specific PresentValue readers are read-only. Cache and PresentValue records
cover published pairs. A best record can show another district in the city.

Status ABI1 retains the first 64 changed samples and exposes trace_count,
trace_capacity (16384) and active. Trace kinds are net/cache/best/pv/late. Each record
contains generation/version, player/city/district/plot, base/result, reason and
eligibility. Net reasons: 0 adjusted, 2 protected, 3 arithmetic rejected,
4 write budget exhausted. Late records use reason 0 for a write and 5/6 for
an entry/final score mismatch. Their base/result fields report the finished
native and resulting scores. Invalid layout or reservation skips the write.
A best district/plot of -1 reports no winner.

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

The policy lock and generation fence prevent older admitted calls from writing or
appending after replacement/reset. This does not make an entire analysis pass
atomic, cancel old requests or remove cached influence. Policy is not serialized;
a consuming mod must reconstruct it after loading a save. The carrier is pinned before hooks become active and remains resident until
process exit. Live FreeLibrary unload is intentionally unsupported; there is no
hook-removal/drain protocol. Multiplayer and general unload safety are unqualified.

### Reproduce the isolated checks

The optional self-test uses synthetic CityBuild buffers and original routines.
It checks both score phases, refusals, generation retirement, JSON paging and
the shared Runtime helper's create/queue/apply failure paths with MinHook stubs.
The test cannot validate actual native memory layout, the MASM entry or Lua
lifetime in a running game.

Open an x64 Developer PowerShell for VS2022 in the DLL project directory.

```powershell
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64') { throw 'Use an x64 Developer PowerShell' }
$ceSource = (Resolve-Path .).Path
$ceTestOutput = Join-Path $env:TEMP 'CE-score-selftest'
New-Item -ItemType Directory -Force -Path $ceTestOutput | Out-Null
Push-Location $ceTestOutput
try {
    cl.exe /nologo /O2 /Gy /EHsc /std:c++20 /DCE_AI_DISTRICT_SCORES_TEST /D_CRT_SECURE_NO_WARNINGS `
        "/I$ceSource" "/I$ceSource\include" "$ceSource\AIDistrictScores.cpp" `
        "$ceSource\native\tests\selftest.cpp" "$ceSource\Runtime.cpp" "$ceSource\HavokScript.cpp" `
        /Fe:ce-score-selftest.exe /link /OPT:REF /NODEFAULTLIB:libMinHook.x64.lib bcrypt.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Self-test compilation failed' }
    .\ce-score-selftest.exe
    if ($LASTEXITCODE -ne 0) { throw 'Self-test checks failed' }
} finally { Pop-Location }
```

Expected results are 166 district-score checks and four hook-group checks.
The instruction-call check exercises byte refusal, near-relay delivery and
restoration with an executable synthetic caller.
The three failure-path diagnostics are expected in the isolated host.

## Earlier C port qualification checkpoint â€” 5 November 2026

Tested implementation: commit `87f48fc7878d00851279bccbc9dd30cfe147470b`, based on
upstream develop `2986a746b396eb93aad14f9afffb043a09c2c489`. The results in
this section apply to that C implementation.

- Existing Release/x64 VCXPROJ builds with VS2022/MSVC14.44.35207, SDK26100 and
  pinned vcpkg dependencies. Unmodified upstream builds in the same environment;
  each reports the same 144 warnings and zero errors. No alternate CE builder or
  source workaround is part of the contribution.
- Feature DLL SHA-256:
  `b1090682fc6c964d077c36efe4dcbac6693d6bf83f9da08b5ad4c0f6e77d139b`.
  All 157 export contracts match upstream; independent static checks cover actual
  imports, compiled guards/bindings/lifecycle, the MASM shim and its unwind data.
- A fresh isolated CSC/Lua test reached turn 100 on Online speed, checked an
  observation-only context at turns 1â€“2, and reloaded the exact save at turn 6,
  using the shared controller. All 101 Lua/Python plan comparisons match across
  100 publications. All 27,775 journal records pass the audit: 62 increases,
  13,746 deductions, all four trace families, zero reported native API faults,
  no original-human trace and read-only diagnostic outputs.
- Normal supported exit (code 0), all 153 subscription preferences, source/package inventories
  and the previous development-package restoration are verified. No Frida,
  Python policy publisher, district orders or grants were used.

The [develop qualification report](https://hennogous.github.io/Henno-Mods/ai/sources/project/docs/research/mods/csc-ce-develop-port-20261105)
identifies the build/native receipts, exact target, test setup and limitations.
The zero-fault count refers to the API and policy. The game logs also contain
duplicate CurrentClickouts localization errors; the report separates new lines
from the pre-run prefix. No general clean-log or attribution claim is made.

This establishes the bounded method on the pinned Windows target, not better AI
quality, a controlled comparison across CSC revisions, Debug, multiplayer,
other CE consumers, live DLL unloading or actual Workshop subscription.

The [earlier prototype report](https://hennogous.github.io/Henno-Mods/ai/sources/project/docs/research/mods/csc-ce-policy-parity-20261104)
preserves the recovered signatures and its distinct runtime observations; its
counts should not be substituted for this develop build. The old PR is closed.
Final capability scope will be reviewed before a replacement PR to develop.

## Builder supplier protection

The optional improvement-score consumer uses the existing Begin, Allow,
Commit and Disable transaction. Stage one or more compatible improvement IDs
per protected site with CE_AIBuilderProtectionAllow(player, city, plot,
improvement). Here Allow means this improvement remains eligible at a listed
site. A site without entries has ordinary scoring. Every publication replaces
both policies. A caller that lacks the optional capability can continue to
publish district entries alone.

CE_AIBuilderProtectionBid(player, city, plot, improvement, deltaQ8) stages
the same allowance with a bounded preference. The accepted range is 0 to
64 score units (16,384 in Q8). The strongest preference for an identical key
wins at publication. The hook adds a bid only to a positive result from the
already eligible native caller, and skips arithmetic overflow. A zero,
negative or ineligible result stays native. Callers can continue to use
CE_AIBuilderProtectionAllow with no preference.

CE_AIBuilderProtectionStatus also reports maxBidQ8, bids and firstBid.
The positive-bid count and first tuple are separate from protection
so a rejected improvement cannot hide evidence of a successful bid.

CE_AIBuilderProtectionStatus returns readiness, allowance count, changed
score count, faults and the first applied tuple with the old and new score
for the current publication. The evidence window rolls at Commit. There is
no per-call journal or journal-dependent write cutoff for this consumer.

The ordinary Win64 function hook at 52a8c0 preserves the original call and
returned pointer. The qualified caller at 3e3423 copies its score unchanged
to the improvement array. For a published site, the hook checks the exact
player, city, coordinates and improvement before substituting the engine's
ineligible score read from b66cb0. Native ranking, pathing, reservations and
job execution remain native. New publications take effect on a fresh score
calculation. Disable retires future writes and does not clear existing
native cache or cancel an assigned job.

This consumer shares the GameCore identity and lifecycle with district
scores. Its own prefix or hook failure leaves the district capability
available. A returned-pointer or output-shape mismatch stops Builder writes
for the context and leaves the original score in place. There is no Lua call
from the AI worker.

## Committed Builder harvest protection

CE_AIBuilderHarvestProtectionStage() opts the current staging transaction
into Clear Plot harvest protection for its Builder allowance sites.
CE_AIBuilderHarvestProtectionStatus returns readiness, opt-in state, fault
count, rejection count and the first [player, plot, worker] receipt.
The opt-in belongs to a publication. Commit resets the receipts. A failed
native write check retires the guard for that context.

The Win64 ClearPlotNode::Update entry at 55eaf0 runs first. Its worker
selection and cleanup remain native. The guard checks the published
generation, admitted AI player, exact protected plot and true harvest
binding. For a ready result it clears the selected worker and returns
the routine's native failure value 3. This prevents the later Clear Plot
execution for that attempt. The tree owns job release and retry. Ordinary
construction and repair with no harvest binding retain the native result.
Other harvest callers, feature removal and non-harvest replacement are
outside this capability.

Target and harvest bindings use the offsets and record widths documented
beside the hook. The selected worker store and player ID read have separate
prefix checks. An unsupported hook leaves score-stage protection available.

## Wonder site protection

CE_AISupplierWonderProtectionStage() opts the staged Builder supplier sites
into Wonder protection. Invoke it after supplying the allowed improvements
and before Commit. It does not stage any additional sites. An empty or
unavailable batch rejects the opt-in. Commit resets the evidence counters.
CE_AISupplierWonderProtectionStatus reports ready, protected, changed, faults
and the first changed tuple (player, city, plot, building, original, result).

The hook uses the standard Win64 building NetValue entry (52a050) and calls
the original with the same eight arguments. It changes a qualified result
only from the coordinate-bearing caller (3e419a, return 3e419f). That caller
checks CanHaveWonder and copies the result directly into the plot row.
The other caller (3e3edd) passes sentinel coordinates for ordinary buildings
and repairs, which retain their original scores. A protected Wonder row
receives the same native ineligible score as a rejected plot. Score changes
leave other sites and the native winner, reservation and production logic
in control. Existing reservations and already cached values are unchanged
until the relevant engine path runs again.

This optional hook uses the same image qualification and shared Runtime
as Builder protection. An unexpected entry, caller or output leaves
ordinary evaluation intact. The publisher and worker share only the staged
immutable site keys. No Lua or world mutation runs from the worker.


## Optional supplemental district repair

`CE_AIDistrictRepairStage(player, city, district, plot)` stages one exact
existing footprint per original AI city inside the district policy
transaction. A publication accepts up to 256 candidates. CSC decides
which repairs are worth making available. The packet carries no score,
priority, or completion command.

The typed `CheckForDistrict` hook at `3e0ee0` runs the stock nomination
first. This version admits a repair beside a newly created stock tree.
It uses the city's `GetDistrictAt` (`11e520`), the existing complete and
pillaged flags, and the `CanZone` call (`0f9d50`) used by `BuildDistrict`.
The exact candidate must have a positive native plot value. Existing
work is checked in the player's active tree list, which the native
manager rebuilds after reading a save. A matching trigger or an active
supplemental tree for the same city blocks another admission.

The stock tree factory (`563840`), controller registration (`53feb0`)
and analysis copy (`0d6160`) own the engine-side objects. The temporary
analysis and display string follow the native stack lifetimes. The
copied trigger must match the original candidate with zero base and
bonus before the manager can run it. The ordinary slot stays with the
stock plan. GameCore decodes the plot value, refreshes and arbitrates
the request, and handles completion and retirement.

`CE_AIDistrictRepairStatus()` exposes readiness, staged capacity,
admitted, duplicate, rejected and fault counts, and the first admitted
tuple `[player,city,plot,tree,controller,plotQ8]` per publication.
Unrecognised native entry bytes disable this optional capability.
Invalid or stale candidates leave stock planning alone. Structural
faults stop new supplemental admissions until context reset. A reset
retires the CE packet and keeps engine-owned requests in the save under
the native manager. Live save/reload behaviour requires a separate test.

### Supplier exclusion in the ordinary district selector

The optional CSC integration stages one owned plot with
`CE_AISupplierExclusionStage(player, city, plot)` in the existing atomic
district batch. `CE_AISupplierExclusionStatus()` reports availability,
capacity and the process lifetime count of rows masked during selection.
The list is limited to 256 distinct plot identities. CSC derives these
identities from live supplier facts once per turn.

The existing `GetBestDistrict` hook checks the published generation and
the current CityBuild identity, bounds, row ownership and row writability.
It borrows eligible bytes for one original call and restores their exact
values immediately afterward. This path uses the existing Win64
`GetBestDistrict` ABI, row stride and eligibility field. It adds no
address, new entry detour, or modified score or reservation cell. The
pinned selector's body traverses these rows without reallocating them and
uses ordinary native reservation checks and tie-breaking.

The mask covers the ordinary joint placement decision. It does not
cancel retained contracts, change the type-specific location helper or
substitute for repair eligibility. The temporary row mask relies on the
stock city activation phase owning its selector view. Broader thread
ownership, callers and Workshop delivery need review before release.
The isolated self-test checks exact byte restoration, nested scope and
unchanged score and reservation values.

### Readback for published guidance

Read receipts before the next commit replaces the publication. Each read is
scoped to the exact policy version, owner and city. A stale version returns
`staged: 0`.

- `CE_AIDistrictScoresReceipt(version, player, city, district, plot)` reads one
  site from the existing bounded journal. It separates score attempts and
  writes, cache observations, calls to the stock city selector and exact
  wins. Cache eligibility is the row's native flag. A recorded win is the
  return from the selector, before reservation, queue and construction.
  Sparse callers can use this API without serializing whole journal pages.
- `CE_AIBuildingNominationReceipt(version, player, city, building)` reports
  forecast calls, ordinary comparison hits and score writes. The read-only
  forecast observer shares the existing building NetValue detour and
  qualifies its ordinary caller at 3e3edd (return 3e3ee2) on the pinned
  XP2 hotfix image. If the caller fingerprint or shared detour is
  unavailable, `forecastReady` is zero. A zero or negative comparison is
  acknowledged and keeps its native value.
- `CE_AIDistrictRepairStatus()` additionally reports how many native
  CheckForDistrict calls reached a city with a staged repair. A call can
  be observed with the normal slot occupied and no supplemental admission.

The stock selector can consider a supplier exclusion even if the same city
has no score edits in that publication. The exclusion still uses the same
owned-row qualification and restores the eligibility byte after the call.

## Optional district purchase ownership acknowledgement

`CE_AIDistrictPurchaseCompletionStage(player, city, district)` adds a generic
numeric scope to Begin/Allow/Commit. A rejected row poisons staging; duplicate
scopes coalesce at commit. Missing/unsupported bytes disable this optional
capability while ordinary score guidance remains available. The caller owns
original-human exclusion and its desired district family.

At ContractManagerDecorator::UpdateContract (55a030), the typed Win64 wrapper
preserves all six arguments and invokes the original once. Only the expected
PurchasePlotContract vtable9fa930 in accepted state3 is considered. Current
executing-tree analysis supplies city/district/plot; a bounded parent walk
checks the root owner-control. Pure map52d00/attributes5f090 (pool4c210) verify
canonical target coordinates, signed owner and purchase-city ID. These must
match that city. No engine pointer is stored between callbacks or across loads.
A lazily unresolved native handle is left to the original getter; a subsequent
update can acknowledge it after native resolution.

The acknowledgement uses archive-aware assignment64a6d0(contract+a0,&seven),
following native BuildingCreated3f81d0 / DistrictCreated3f8210. This is a generic
tracked setter, not a CompleteContract API; direct contract+b0 writes would
bypass archive dirtiness. The normal decorator still handles progression and
cleanup. The short setter is called under the publication lock only because
its audited body has no AI/HKS callbacks; original functions and pure lookups
run outside that lock. A generation mismatch or inconsistent identity prevents
the write. A failed state transition disables further acknowledgements until
context reset.

`CE_AIDistrictPurchaseCompletionStatus()` reports readiness, scope count,
considerations, acknowledgements and faults. The versioned
`CE_AIDistrictPurchaseCompletionReceipt(version, player, city, district)` reads
that exact scope's considered/owned/acknowledged/rejected counts plus last
contract/plot/before/after, independent of clipped samples. Ownership fulfilment
is not payment evidence: cultural growth can also satisfy the prerequisite.
No funds, scores, purchases, reservations or build commands are supplied here.

Binary/ABI evidence and controlled diagnostic outcome: AI-N118 and
[CSC acquisition evidence](https://github.com/hennogous/Henno-Mods/blob/main/project/docs/research/mods/csc-ai-nuremberg-acquisition-20261112.md).
The 13 added native checks exercise disabled/out-of-scope, unowned/other-city,
invalid identities, tracked completion/deduplication, exact/stale receipts,
staging rejection and context retirement. They do not establish live ABI,
reload reconstruction or integrated construction; those remain live gates.

### Typed district-site declines

This optional ABI1 uses the existing joint district selector hook and its
already qualified CityBuild matrix. It adds no native address or detour.

- Begin/Allow establish version, dimensions and original-AI identity.
- CE_AIDistrictDeclineStage(player, city, district, plot) stages one exact cell.
- Commit sorts/deduplicates the bounded list (capacity4096) and atomically
  replaces it with the rest of the complete policy. Failed staging poisons
  that transaction; callers must disable or handle failure as for score bids.
- CE_AIDistrictDeclineStatus reports capability/version/scope capacity and
  considered, masked, restored, rejected and fault counts.
- CE_AIDistrictDeclineReceipt(version, player, city, district, plot) reads one
  exact key. Stale version is staged0. Ready alone does not mean enabled;
  pair it with the normal policy status.

Decline is choice admission, not an economic score or a production order.
Only during original_best, eligible owned/frontier cells in the valid score
range are temporarily set to DeclinedScore = InvalidScore - 1. The native
predicate is reservationPriority < cell && currentBest <= cell; currentBest
starts at InvalidScore, so equality with that floor is insufficient for a
low-priority reserved row. Original cells at the floor also need the temporary
mask; below-floor and explicitly protected extreme values remain untouched.
Reservation handles and other district columns are unchanged. The RAII scope
restores every original i32 before ordinary cache/Best readback, even when
publication retires during the call. Nested masks cannot restore their parent.
The selector does not relocate/free its matrix during this qualified call.

Mask storage is bounded to this city's published keys. Allocation failure
before any write retires the capability and selects normally; journal capacity
is checked for the entire mask batch before any cell changes. No Lua is called
from native jobs. Tests cover nested/retired scopes, owned/frontier/reserved
rows, zero/negative/floor scores, native predicate boundaries, protected values,
detached row pointers and two-cell exact/insufficient journal capacity.

Receipt meanings are deliberately distinct: considered is consultation, not
admission. Missing, ineligible or protected cells can be considered with no
mask and no rejection. Rejected reports the pointer/write-layout guard.
originalQ8 is meaningful only after masked>0. Restoration after retirement
does not credit a replacement generation's counters. All counters reset at
the next complete commit; archive observations before replacement.

The mod owns the semantic reason for declining a site and must withdraw it
when live facts improve. The API cannot verify supply potential or benefit.
It does not cancel a retained tree, invalidate a permanent cache, choose a
different district or alter later competition with ordinary production.
