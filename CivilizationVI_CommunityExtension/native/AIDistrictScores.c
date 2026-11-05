/* CE district-site score API. Read native/README.md for publication and paging.
 *
 * There are two execution domains: gameplay Lua stages a complete numeric policy;
 * native AI jobs copy an admitted entry, run the original game routine, and apply
 * at most one adjustment if the publication generation still matches. AI hooks
 * never invoke Lua/gameplay APIs. The engine owns every object/output pointer;
 * CE owns only the fixed-size policy, samples and trace journal below.
 *
 * This module is compiled into CE. Its integration points must be called by CE's
 * process-once initializer and context lifecycle, not by an addon/DLL loader. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "MinHook.h"
#include "target_identity.h"
#include "AIDistrictScores.h"

#define API_NAME(suffix) "CE_AIDistrictScores" suffix

/* Inherited diagnostic limits, not tuning recommendations. Native utility is
 * Q8: 256 raw units = 1 utility. INVALID_SCORE is the engine's -9999 sentinel;
 * the policy's -4096 floor stays above it. The API permits -1024..+4096 utility
 * per entry, 4096 entries/writes and a finite 16384-record evidence window. */
#define CAPACITY 4096
#define INVALID_SCORE (-2559744)
#define TRACE_CAPACITY 16384
#define TRACE_PAGE 64
typedef struct {
    int player, city, district, plot, delta;
} Entry;
typedef struct {
    Entry key;
    int base, result;
} Sample;
typedef struct {
    int version, width, height, districts, count, allowed[64];
    Entry entries[CAPACITY];
} Policy;
/* One publisher per context. No per-mod ownership token is provided: consumers
 * must coordinate rather than overwrite one another's global policy. The lock
 * protects staging, published policy, admission flags and journal. Never hold it
 * across an original game call or HKS operation (which may re-enter/longjmp). */
static SRWLOCK lock = SRWLOCK_INIT;
/* Staging is invisible to workers. Commit copies it under the exclusive lock;
 * workers retain Entry values, never pointers into a table that can be replaced. */
static Policy staging, policy;
static LONG enabled, generation, calls, matched, changed, faults;
static int staging_valid, sample_count, hooks_ready;
static Sample samples[64];
/* Bounded diagnostic journal. Every record is numeric data captured by a native
 * hook; records are pulled by gameplay Lua, never a callback into Lua. */
typedef struct {
    int kind, generation, version, player, city, district, plot;
    int base, result, reason, eligible;
} TraceRecord;
typedef struct {
    int accepted, epoch;
    Entry key;
} Observation;
static TraceRecord trace_records[TRACE_CAPACITY];
static int trace_count;
/* Unlike per-publication counters, active is not cleared by reset: older calls
 * still have to decrement it when they return. calls is cumulative hook traffic. */
static LONG active;
/* Recovered and observed Windows-x64 signatures for the pinned XP2 target:
 * CalculateDistrictValues(this) -> void; GetBestDistrict(this, int32 xy[2]) ->
 * district index; PresentValueOf(this, int32 *out, district, optional Plot*) ->
 * the same out pointer. These are NOT portable CE object layouts/ABIs. */
typedef void (__cdecl *CacheFn)(void *);
typedef int (__cdecl *BestFn)(void *, int *);
typedef int *(__cdecl *PresentFn)(void *, int *, int, void *);
static CacheFn original_cache;
static BestFn original_best;
static PresentFn original_present;
/* Borrowed from CE Runtime. The CE carrier is pinned before enabling hooks;
 * CE must retain the original GameCore for the callbacks' process lifetime. */
static HMODULE original;
static uintptr_t original_base;
/* NetValue(out, district, timeOut, ready, horizon, Plot*, City*) returns out.
 * hook_entry.asm preserves caller R12 and the return address as extra arguments
 * for bridge_net only. Neither is passed as an extra argument to the original. */
typedef int *(__cdecl *NetFn)(int *, unsigned, int *, int, int, void *, void *);
static NetFn original_net;
extern void bridge_net_entry(void);

/* Resolve only public HKS exports. No copied HKS structs or CE implementation. */
typedef int (__cdecl *LuaFn)(void *);
static void (__cdecl *push_closure)(void *, LuaFn, int, const char *, int);
static void (__cdecl *set_field)(void *, int, const char *);
static int (__cdecl *check_integer)(void *, int);
static void (__cdecl *push_integer)(void *, int);
static const char *(__cdecl *push_string)(void *, const char *, ...);
static void (__cdecl *concat_strings)(void *, int);
static void (__cdecl *check_stack)(void *, int, const char *);
/* The qualified HKS formatter truncates a payload at 511 bytes; no free
 * lua_pushlstring export was found. Our JSON has fixed ASCII keys and integers,
 * no embedded NULs. A 240-byte literal fragment stays well below that limit.
 * Incremental concat needs only two temporary stack values, independent of JSON
 * length. HKS may raise an error here, so all callers release the SRW lock first.
 * Always use "%s": the JSON must never become a printf formatting program. */
static void push_json(void *L, const char *text, size_t length) {
    char fragment[241];
    size_t offset = 0;
    check_stack(L, 2, "CE district-score JSON");
    do {
        size_t count = length - offset;
        if (count > 240)
            count = 240;
        memcpy(fragment, text + offset, count);
        fragment[count] = 0;
        push_string(L, "%s", fragment);
        if (offset)
            concat_strings(L, 2);
        offset += count;
    } while (offset < length);
}

/* Validate the whole borrowed span within one committed, accessible region.
 * Rejecting a cross-region span is conservative. VirtualQuery is not a lifetime
 * lock: it cannot prevent unrelated engine code from freeing memory afterward.
 * Correct native-call ownership and the pinned-layout contract remain required. */
static int readable(const void *p, size_t bytes, int write) {
    MEMORY_BASIC_INFORMATION m;
    uintptr_t start = (uintptr_t)p, end = start + bytes;
    if (!p || !bytes || end < start || VirtualQuery(p, &m, sizeof m) != sizeof m ||
        m.State != MEM_COMMIT || (m.Protect&PAGE_GUARD) || end > (uintptr_t)m.BaseAddress + m.RegionSize)
        return 0;
    DWORD mode = m.Protect&255;
    return mode == PAGE_READWRITE || mode == PAGE_WRITECOPY || mode == PAGE_EXECUTE_READWRITE ||
        mode == PAGE_EXECUTE_WRITECOPY || (!write && (mode == PAGE_READONLY || mode == PAGE_EXECUTE_READ));
}

/* Default to the original value on every rejection. Use int64 for addition,
 * preserve native zero/invalid results, and never raise a low score while trying
 * to penalize it. INT_MAX/2 is conservative headroom for the known later 1.5
 * district preference factor, not a proof about every downstream computation. */
static int adjusted(int base, int delta, int *result) {
    int64_t sum = (int64_t)base + delta;
    *result = base;
    if (!base || base <= INVALID_SCORE || (delta < 0 && base <= -1048576))
        return 0;
    if (delta < 0 && sum < -1048576)
        sum = -1048576;
    /* Retain room for the native positive district preference factor. */
    if (sum > INT_MAX/2 || sum < INT_MIN || sum == INVALID_SCORE)
        return 0;
    *result = (int)sum;
    return 1;
}

/* Called only while the exclusive policy lock is held. Diagnostic exhaustion
 * disables further incentives rather than losing the relationship between a
 * native write and its evidence. Published policy is never changed by a hook. */
static void native_fault(void) {
    faults++;
    enabled = 0;
}
static int trace_room(void) {
    if (trace_count >= TRACE_CAPACITY) {
        native_fault();
        return 0;
    }
    return 1;
}
static void record_trace(int kind, Entry key, int district, int plot, int base, int result, int reason, int eligible) {
    if (!trace_room())
        return;
    trace_records[trace_count++] = (TraceRecord){kind, generation, policy.version,
    key.player, key.city, district, plot, base, result, reason, eligible};
}
/* Both lookup helpers require the caller's shared/exclusive policy lock.
 * Returned addresses may be used only while that lock is held; hook admissions
 * copy the Entry so no policy-table pointer crosses an original game call. */
static Entry *find_pair(int player, int city, int district, int plot) {
    if (player < 0 || player >= 64 || !policy.allowed[player])
        return NULL;
    for (int i = 0; i < policy.count; i++) {
        Entry *e = &policy.entries[i];
        if (e->player == player && e->city == city && e->district == district && e->plot == plot)
            return e;
    }
    return NULL;
}
static Entry *find_city(int player, int city) {
    if (player < 0 || player >= 64 || !policy.allowed[player])
        return NULL;
    for (int i = 0; i < policy.count; i++) {
        Entry *e = &policy.entries[i];
        if (e->player == player && e->city == city)
            return e;
    }
    return NULL;
}
/* Admit read-only tracing only for a city represented in the current policy.
 * CityBuild+90 -> City; City+d8/a8 -> owner/local city ID. Those offsets, and all
 * offsets below, were recovered for the exact Windows XP2 image in README. */
static Observation observe_enter(void *object) {
    Observation o = {0};
    InterlockedIncrement(&calls);
    AcquireSRWLockShared(&lock);
    if (enabled && readable(object, 0x4dc, 0)) {
        unsigned char *city = *(unsigned char **)((unsigned char *)object + 0x90);
        if (readable(city, 0xdc, 0)) {
            Entry *e = find_city(*(int *)(city + 0xd8), *(int *)(city + 0xa8));
            if (e) {
                o.accepted = 1;
                o.epoch = generation;
                o.key = *e;
            }
        }
    }
    ReleaseSRWLockShared(&lock);
    return o;
}
/* Called with the exclusive policy lock AFTER the original routine returned.
 * CityBuild+4c8/4d8 is a <=37-row array, stride 78h. Each row's +20 points to its
 * int32 district scores; +8/+c are coordinates. +55 admits districts, while
 * +5a/+57 means owned/frontier eligibility. The identity is checked again because
 * a generation fence is not a memory-lifetime guarantee.
 *
 * kind 1 records published pairs in the completed cache. kind 2 records the
 * actual winning pair even if it is another district in the scoped city. This
 * is a return-boundary observation, not an atomic whole-analysis snapshot. */
static void observe_cache(void *object, Observation o, int kind, int district, int plot) {
    if (!enabled || generation != o.epoch)
        return;
    unsigned char *b = object;
    if (!readable(b, 0x4dc, 0)) {
        native_fault();
        return;
    }
    unsigned char *city = *(unsigned char **)(b + 0x90);
    if (!readable(city, 0xdc, 0) || *(int *)(city + 0xd8) != o.key.player || *(int *)(city + 0xa8) != o.key.city) {
        native_fault();
        return;
    }
    unsigned n = *(unsigned *)(b + 0x4d8);
    unsigned char *rows = *(unsigned char **)(b + 0x4c8);
    if (n > 37 || (n && !readable(rows, (size_t)n*0x78, 0))) {
        native_fault();
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        unsigned char *row = rows + (size_t)i*0x78;
        int x = *(int *)(row + 8), y = *(int *)(row + 12);
        if (x < 0 || y < 0 || x >= policy.width || y >= policy.height)
            continue;
        int index = y*policy.width + x;
        if (kind == 2 && index != plot)
            continue;
        int *scores = *(int **)(row + 0x20);
        if (!readable(scores, (size_t)policy.districts*4, 0)) {
            native_fault();
            return;
        }
        int eligible = !!(row[0x55] && (row[0x5a] || row[0x57]));
        if (kind == 2) {
            record_trace(kind, o.key, district, plot, scores[district], scores[district], 0, eligible);
            return;
        }
        for (int j = 0; j < policy.count; j++) {
            Entry e = policy.entries[j];
            if (e.player == o.key.player && e.city == o.key.city && e.plot == index) {
                record_trace(kind, e, e.district, index, scores[e.district], scores[e.district], 0, eligible);
                if (!enabled)
                    return;
            }
        }
    }
}
/* Every wrapper retains native return/output behavior. The active counter spans
 * the original call; the SRW lock deliberately does not. Nested NetValue calls
 * during cache calculation can therefore acquire the same lock safely. */
static void __cdecl hook_cache(void *object) {
    InterlockedIncrement(&active);
    Observation o = observe_enter(object);
    original_cache(object);
    if (o.accepted) {
        AcquireSRWLockExclusive(&lock);
        observe_cache(object, o, 1, 0, 0);
        ReleaseSRWLockExclusive(&lock);
    }
    InterlockedDecrement(&active);
}
/* A -1 return is a legitimate no-winner result; coordinates are undefined then.
 * For a winner, read exactly two int32 coordinates and keep the district return
 * unchanged. Observing a winner does not establish a request or construction. */
static int __cdecl hook_best(void *object, int *coordinates) {
    InterlockedIncrement(&active);
    Observation o = observe_enter(object);
    int result = original_best(object, coordinates);
    if (o.accepted) {
        AcquireSRWLockExclusive(&lock);
        if (enabled && generation == o.epoch) {
            if (result == -1)
                record_trace(2, o.key, -1, -1, 0, 0, 0, 0);
            else if (result < 0 || result >= policy.districts || !readable(coordinates, 8, 0))
                native_fault();
            else {
                int x = coordinates[0], y = coordinates[1];
                if (x < 0 || y < 0 || x >= policy.width || y >= policy.height)
                    native_fault();
                else observe_cache(object, o, 2, result, y*policy.width + x);
            }
        }
        ReleaseSRWLockExclusive(&lock);
    }
    InterlockedDecrement(&active);
    return result;
}
/* Match the old observer's plot-specific scope. A null Plot* selects a different
 * native path/meaning and is intentionally not traced here. The original
 * routine must return its caller-owned out pointer; never substitute a pointer. */
static int *__cdecl hook_present(void *object, int *out, int district, void *plot) {
    InterlockedIncrement(&active);
    Observation o = observe_enter(object);
    AcquireSRWLockShared(&lock);
    if (o.accepted && enabled && generation == o.epoch && district > 0 && district < policy.districts && readable(plot, 0x2c, 0)) {
        unsigned char *p = plot;
        int x = *(short *)(p + 0x28), y = *(short *)(p + 0x2a);
        Entry *e = (x >= 0 && y >= 0 && x < policy.width && y < policy.height)?find_pair(o.key.player, o.key.city, district, y*policy.width + x):NULL;
        if (e)
            o.key = *e;
        else o.accepted = 0;
    } else o.accepted = 0;
    ReleaseSRWLockShared(&lock);
    int *returned = original_present(object, out, district, plot);
    if (o.accepted) {
        AcquireSRWLockExclusive(&lock);
        if (enabled && generation == o.epoch) {
            if (returned != out || !readable(out, 4, 0))
                native_fault();
            else record_trace(3, o.key, district, o.key.plot, *out, *out, 0, 1);
        }
        ReleaseSRWLockExclusive(&lock);
    }
    InterlockedDecrement(&active);
    return returned;
}
/* Mutation is restricted to the ordinary caller returning to RVA 3e30be.
 * Other calls to NetValue retain native behavior. The shim supplies caller R12,
 * which is the plot-analysis row at this exact call site. Compare row coords,
 * owner/city IDs and native eligibility before copying an admitted Entry.
 *
 * After the original call, the copied generation must still match. Disable,
 * Begin, Commit or Reset may have retired it while the game routine ran. That
 * fence suppresses the OLD invocation's write; it does not make all rows in a
 * city-analysis pass use one policy or roll back saved cache/request influence. */
int *bridge_net(int *out, unsigned district, int *time, int ready, int horizon,
void *plot, void *city, unsigned char *row, void *caller) {
    Entry entry;
    int found = 0, epoch = 0;
    InterlockedIncrement(&active);
    InterlockedIncrement(&calls);
    if (InterlockedCompareExchange(&enabled, 0, 0) && caller == (void *)(original_base + 0x3e30be)) {
        AcquireSRWLockShared(&lock);
        if (enabled && readable(city, 0xdc, 0) && readable(plot, 0x2c, 0) && readable(row, 0x78, 0)) {
            int player = *(int *)((char *)city + 0xd8), cid = *(int *)((char *)city + 0xa8);
            int x = *(short *)((char *)plot + 0x28), y = *(short *)((char *)plot + 0x2a);
            if (player >= 0 && player < 64 && policy.allowed[player] && x >= 0 && x < policy.width && y >= 0 && y < policy.height &&
                *(int *)(row + 8) == x && *(int *)(row + 12) == y && *(short *)(row + 0x70) == player &&
                *(int *)(row + 0x74) == cid && row[0x55] && (row[0x5a] || row[0x57])) {
                Entry *e = find_pair(player, cid, (int)district, y*policy.width + x);
                if (e) {
                    entry = *e;
                    epoch = generation;
                    found = 1;
                }
            }
        }
        ReleaseSRWLockShared(&lock);
    }
    int *returned = original_net(out, district, time, ready, horizon, plot, city);
    if (found) {
        AcquireSRWLockExclusive(&lock);
        if (enabled && generation == epoch) {
            matched++;
            if (returned != out || !readable(out, 4, 1))
                native_fault();
            else if (trace_room()) {
                int base = *out, result = base, reason = 0;
                if (!adjusted(base, entry.delta, &result))
                reason = (!base || base <= INVALID_SCORE || (entry.delta < 0 && base <= -1048576))?2:3;
                else if (changed >= 4096) {
                    native_fault();
                    reason = 4;
                    result = base;
                }
                else {
                    *out = result;
                    changed++;
                    if (sample_count < 64)
                        samples[sample_count++] = (Sample){entry, base, result};
                }
                record_trace(0, entry, entry.district, entry.plot, base, result, reason, 1);
            }
        }
        ReleaseSRWLockExclusive(&lock);
    }
    InterlockedDecrement(&active);
    return returned;
}

static int integer_result(void *L, int value) {
    push_integer(L, value);
    return 1;
}
/* Convert every Lua argument BEFORE taking a lock: HKS checkinteger can longjmp
 * on an argument error. Consumers must catch errors and Disable explicitly.
 * Once conversion succeeds, Begin retires the old policy even if RANGE/version
 * validation fails. Argument errors before this point leave prior state intact. */
static int l_begin(void *L) {
    int v = check_integer(L, 1), w = check_integer(L, 2), h = check_integer(L, 3), d = check_integer(L, 4), ok;
    AcquireSRWLockExclusive(&lock);
    enabled = 0;
    generation++;
    ok = v > policy.version && w > 0 && w <= 256 && h > 0 && h <= 256 && d > 0 && d <= 4096;
    memset(&staging, 0, sizeof staging);
    staging_valid = ok;
    if (ok) {
        staging.version = v;
        staging.width = w;
        staging.height = h;
        staging.districts = d;
    }
    ReleaseSRWLockExclusive(&lock);
    return integer_result(L, ok);
}
/* A rejected Allow/Entry poisons the staging batch. A later Commit cannot make a
 * partially validated table visible; the caller must start a new Begin. */
static int l_allow(void *L) {
    int p = check_integer(L, 1), ok;
    AcquireSRWLockExclusive(&lock);
    ok = staging_valid && p >= 0 && p < 64;
    if (ok)
        staging.allowed[p] = 1;
    else
        staging_valid = 0;
    ReleaseSRWLockExclusive(&lock);
    return integer_result(L, ok);
}
/* City IDs are scoped to players; the full four-field key identifies a pair.
 * The caller owns original-human exclusion, legality and world-state freshness.
 * The native hook supplies additional identity/row guards, not an AI-role oracle. */
static int l_entry(void *L) {
    Entry e = {check_integer(L, 1), check_integer(L, 2), check_integer(L, 3), check_integer(L, 4), check_integer(L, 5)};
    AcquireSRWLockExclusive(&lock);
    int ok = staging_valid && staging.count < CAPACITY && e.player >= 0 && e.player < 64 && staging.allowed[e.player] &&
        e.city >= 0 && e.district > 0 && e.district < staging.districts && e.plot >= 0 && e.plot < staging.width*staging.height &&
        e.delta >= -262144 && e.delta <= 1048576 && e.delta != 0;
    for (int i = 0; ok && i < staging.count; i++) {
        Entry a = staging.entries[i];
        if (a.player == e.player && a.city == e.city && a.district == e.district && a.plot == e.plot)
            ok = 0;
    }
    if (ok)
        staging.entries[staging.count++] = e;
    else
        staging_valid = 0;
    ReleaseSRWLockExclusive(&lock);
    return integer_result(L, ok);
}
/* Only a ready, fault-free consumer may publish. Faults stay sticky until a
 * context reset. Clearing trace/samples here starts a new evidence window, so
 * callers must archive the retired window before committing a replacement. */
static int l_commit(void *L) {
    AcquireSRWLockExclusive(&lock);
    int ok = staging_valid && hooks_ready && !faults;
    if (ok) {
        policy = staging;
        changed = matched = sample_count = trace_count = 0;
        generation++;
        enabled = 1;
    }
    staging_valid = 0;
    ReleaseSRWLockExclusive(&lock);
    return integer_result(L, ok);
}
/* Retire without clearing the journal or published version. Advancing generation
 * prevents already admitted callbacks from appending/writing when they return.
 * They may still be executing their original game routine; this is not a drain. */
static int l_disable(void *L) {
    AcquireSRWLockExclusive(&lock);
    enabled = 0;
    staging_valid = 0;
    generation++;
    ReleaseSRWLockExclusive(&lock);
    return integer_result(L, 1);
}
/* Serialize fixed-size numeric fields while holding a shared lock; release it
 * before touching HKS. 64 samples of six int32s fit comfortably in 16000 bytes.
 * Reassess that bound if adding variable strings or increasing sample count. */
static int l_status(void *L) {
    char output[16000];
    AcquireSRWLockShared(&lock);
    int n = snprintf(
        output,
        sizeof output,
        "{\"abi\":1,\"ready\":%d,\"enabled\":%ld,\"generation\":%ld,\"version\":%d,\"entries\":%d,\"calls\":%ld,\"matched\":%ld,\"changed\":%ld,\"faults\":%ld,\"samples\":[",
        hooks_ready,
        enabled,
        generation,
        policy.version,
        policy.count,
        calls,
        matched,
        changed,
        faults
    );
    for (int i = 0; i < sample_count; i++) {
        Sample s = samples[i];
        n += snprintf(
            output + n,
            sizeof output - n,
            "%s[%d,%d,%d,%d,%d,%d]",
            i?",":"",
            s.key.player,
            s.key.city,
            s.key.district,
            s.key.plot,
            s.base,
            s.result
        );
    }
    snprintf(
        output + n,
        sizeof output - n,
        "],\"trace_count\":%d,\"trace_capacity\":%d,\"active\":%ld}",
        trace_count,
        TRACE_CAPACITY,
        InterlockedCompareExchange(&active, 0, 0)
    );
    ReleaseSRWLockShared(&lock);
    push_json(L, output, strlen(output));
    return 1;
}
/* Non-consuming zero-based pages; expected generation prevents a caller from
 * accidentally joining records across a publication/context reset. Disable and
 * capture before replacement when a stable complete window is required. Disable
 * changes generation, so old callbacks may finish but cannot append or mutate. */
static int l_trace_count(void *L) {
    AcquireSRWLockShared(&lock);
    int count = trace_count;
    ReleaseSRWLockShared(&lock);
    return integer_result(L, count);
}
/* Copy at most 64 records under the lock, then format/push outside it. Integer
 * field widths and fixed keys bound the JSON well below 32768 bytes. A stale
 * generation returns no records. While enabled, later callbacks may append
 * between pages; Disable first for a complete stable publication window. */
static int l_trace_read(void *L) {
    int offset = check_integer(L, 1), limit = check_integer(L, 2), expected = check_integer(L, 3);
    char output[32768];
    TraceRecord page[TRACE_PAGE];
    int count = 0, total, epoch, version, stale;
    if (offset < 0 || offset > TRACE_CAPACITY || limit < 1 || limit > TRACE_PAGE || expected < 0) {
        push_json(L, "{\"error\":\"invalid_trace_page\"}", strlen("{\"error\":\"invalid_trace_page\"}"));
        return 1;
    }
    AcquireSRWLockShared(&lock);
    total = trace_count;
    epoch = generation;
    version = policy.version;
    stale = expected != epoch;
    if (!stale && offset < total) {
        count = total - offset;
        if (count > limit)
            count = limit;
        memcpy(page, trace_records + offset, (size_t)count*sizeof *page);
    }
    ReleaseSRWLockShared(&lock);
    int n = snprintf(
        output,
        sizeof output,
        "{\"generation\":%d,\"version\":%d,\"total\":%d,\"offset\":%d,\"next\":%d,\"stale\":%s,\"records\":[",
        epoch,
        version,
        total,
        offset,
        offset + count,
        stale?"true":"false"
    );
    static const char *kinds[] = {"net", "cache", "best", "pv"};
    for (int i = 0; i < count; i++) {
        TraceRecord r = page[i];
        n += snprintf(
            output + n,
            sizeof output - n,
            "%s{\"k\":\"%s\",\"generation\":%d,\"version\":%d,\"player\":%d,\"city\":%d,\"district\":%d,\"plot\":%d,\"base\":%d,\"result\":%d,\"reason\":%d,\"eligible\":%d}",
            i?",":"",
            kinds[r.kind],
            r.generation,
            r.version,
            r.player,
            r.city,
            r.district,
            r.plot,
            r.base,
            r.result,
            r.reason,
            r.eligible
        );
    }
    snprintf(output + n, sizeof output - n, "]}");
    push_json(L, output, strlen(output));
    return 1;
}
/* CE calls this for each gameplay Lua state during normal registration. The
 * state and its stack belong to HKS; no Lua state pointer is retained by workers. */
static void register_bindings(void *L) {
    struct {
        const char *name;
        LuaFn fn;
    } bindings[] = {
 {API_NAME("Begin"), l_begin}, {API_NAME("Allow"), l_allow}, {API_NAME("Entry"), l_entry},
 {API_NAME("Commit"), l_commit}, {API_NAME("Disable"), l_disable}, {API_NAME("Status"), l_status},
 {API_NAME("TraceCount"), l_trace_count}, {API_NAME("TraceRead"), l_trace_read}};
    for (unsigned i = 0; i < sizeof bindings/sizeof bindings[0]; i++) {
        push_closure(L, bindings[i].fn, 0, bindings[i].name, 0);
        set_field(L, -10002, bindings[i].name);
    }
}

/* Qualify CE's on-disk original independently of names/RVAs. Handles and the
 * BCrypt hash object are local and released on every exit; no file is modified.
 * Loaded metadata/prefix checks below additionally qualify the in-memory image. */
static int verify_file(const wchar_t *path) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    DWORD object_size = 0, received = 0;
    unsigned char digest[32], buffer[65536];
    char hex[65];
    int ok = 0;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
        goto done;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_size, sizeof object_size, &received, 0) < 0)
        goto done;
    unsigned char *object = HeapAlloc(GetProcessHeap(), 0, object_size);
    if (!object)
        goto done;
    if (BCryptCreateHash(alg, &hash, object, object_size, NULL, 0, 0) >= 0) {
        ok = 1;
        for (; ; ) {
            DWORD count;
            if (!ReadFile(file, buffer, sizeof buffer, &count, NULL)) {
                ok = 0;
                break;
            }
            if (!count)
                break;
            if (BCryptHashData(hash, buffer, count, 0) < 0) {
                ok = 0;
                break;
            }
        }
        if (ok)
            ok = BCryptFinishHash(hash, digest, 32, 0) >= 0;
        BCryptDestroyHash(hash);
    }
    HeapFree(GetProcessHeap(), 0, object);
    if (ok) {
        for (int i = 0; i < 32; i++)
            sprintf(hex + 2*i, "%02x", digest[i]);
        ok = !strcmp(hex, CE_AI_DISTRICT_GAMECORE_SHA256);
    }
done:
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    return ok;
}

/* Recovered function entries for the SHA-qualified Windows XP2 image. These exact
 * prefixes were checked by the previous four-hook experiment; mapped function
 * names alone are not signatures. A new GameCore build requires fresh ABI, field,
 * call-site and prefix qualification; do not merely update the file hash. */
static const unsigned char fingerprint_cache[32] = {0x4c, 0x8b, 0xdc, 0x41, 0x55, 0x48, 0x81, 0xec, 0xa0, 0x00, 0x00, 0x00, 0x83, 0xb9, 0xd8, 0x04, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0xe9, 0x0f, 0x84, 0xae, 0x03, 0x00, 0x00, 0x48, 0x8b, 0x91, 0xc8};
static const unsigned char fingerprint_best[32] = {0x48, 0x89, 0x4c, 0x24, 0x08, 0x53, 0x55, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83, 0xec, 0x28, 0x4c, 0x8b, 0xea, 0x48, 0x8b, 0xe9, 0xe8, 0x73, 0xbe, 0x3e, 0x00, 0x48, 0x8b, 0xb5};
static const unsigned char fingerprint_present[32] = {0x4d, 0x63, 0xd8, 0x4c, 0x8b, 0xd2, 0x48, 0x8b, 0xc1, 0x41, 0x83, 0xfb, 0xff, 0x75, 0x08, 0x33, 0xc9, 0x48, 0x8b, 0xc2, 0x89, 0x0a, 0xc3, 0x4d, 0x85, 0xc9, 0x74, 0x4d, 0x8b, 0x81, 0xd8, 0x04};
/* CE invokes this once, before its own InitHooks changes RegisterScriptData.
 * The registration prefix is a QUALIFICATION guard, not a fifth detour here.
 * Resolve the complete HKS transport and verify every target before installing
 * any of our hooks. No city analysis may be running during this initialization. */
static int install_hooks(void) {
    if (hooks_ready)
        return 1;
    unsigned char *base = (unsigned char *)original;
    IMAGE_DOS_HEADER *dos = (void *)base;
    IMAGE_NT_HEADERS64 *pe = (void *)(base + dos->e_lfanew);
    if (pe->FileHeader.TimeDateStamp != CE_AI_DISTRICT_GAMECORE_TIMESTAMP || pe->OptionalHeader.SizeOfImage != CE_AI_DISTRICT_GAMECORE_IMAGE_SIZE ||
        memcmp(base + 0x5bdac0, fingerprint_register, 32) || memcmp(base + 0x52a440, fingerprint_net, 32) ||
        memcmp(base + 0x3dfee0, fingerprint_cache, sizeof fingerprint_cache) ||
        memcmp(base + 0x3e4a50, fingerprint_best, sizeof fingerprint_best) ||
        memcmp(base + 0x3e7f40, fingerprint_present, sizeof fingerprint_present))
        return 0;
    HMODULE hks = GetModuleHandleW(L"HavokScript_FinalRelease.dll");
    if (!hks)
        return 0;
    push_closure = (void *)GetProcAddress(hks, "?hks_pushnamedcclosure@@YAXPEAUlua_State@@P6AH0@ZHPEBDH@Z");
    set_field = (void *)GetProcAddress(hks, "?hksi_lua_setfield@@YAXPEAUlua_State@@HPEBD@Z");
    check_integer = (void *)GetProcAddress(hks, "?luaL_checkinteger@@YAHPEAUlua_State@@H@Z");
    push_integer = (void *)GetProcAddress(hks, "?hksi_lua_pushinteger@@YAXPEAUlua_State@@H@Z");
    push_string = (void *)GetProcAddress(hks, "?hksi_lua_pushfstring@@YAPEBDPEAUlua_State@@PEBDZZ");
    concat_strings = (void *)GetProcAddress(hks, "?hksi_lua_concat@@YAXPEAUlua_State@@H@Z");
    check_stack = (void *)GetProcAddress(hks, "?luaL_checkstack@@YAXPEAUlua_State@@HPEBD@Z");
    if (!push_closure || !set_field || !check_integer || !push_integer || !push_string || !concat_strings || !check_stack)
        return 0;
    /* Pin the carrier before enabling callbacks. This intentionally prevents normal
     * FreeLibrary unload; no live-hook removal/drain protocol is implemented.
     * Context recreation resets policy while these trampolines stay resident. */
    HMODULE pinned;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)&install_hooks, &pinned))
        return 0;
    struct {
        uintptr_t rva;
        void *detour;
        void **trampoline;
    } hooks[] = {
 {0x52a440, (void *)&bridge_net_entry, (void **)&original_net},
 {0x3dfee0, (void *)&hook_cache, (void **)&original_cache},
 {0x3e4a50, (void *)&hook_best, (void **)&original_best},
 {0x3e7f40, (void *)&hook_present, (void **)&original_present}};
    unsigned created = 0;
    for (; created < sizeof hooks/sizeof hooks[0]; created++) {
        if (MH_CreateHook(base + hooks[created].rva, hooks[created].detour, hooks[created].trampoline) != MH_OK)
            goto hook_failure;
    }
    /* No city analysis exists before the original context is created. */
    for (unsigned i = 0; i < created; i++)
        if (MH_QueueEnableHook(base + hooks[i].rva) != MH_OK)
            goto hook_failure;
    if (MH_ApplyQueued() != MH_OK)
        goto hook_failure;
    hooks_ready = 1;
    return 1;
hook_failure:
    /* Best-effort removal of THIS module's hooks only; never remove CE's hooks.
     * ready remains false, so no policy can commit even after partial failure. */
    for (unsigned i = 0; i < created; i++) {
        MH_DisableHook(base + hooks[i].rva);
        MH_RemoveHook(base + hooks[i].rva);
    }
    return 0;
}
/* Context boundary: retire admission before clearing policy/version/evidence.
 * Old callbacks keep only value copies and fail their generation check. Preserve
 * active/calls because old frames may still return and decrement active. This
 * does not undo engine-owned cached values or retained requests from the save. */
static void reset_policy(void) {
    AcquireSRWLockExclusive(&lock);
    enabled = 0;
    staging_valid = 0;
    generation++;
    memset(&policy, 0, sizeof policy);
    changed = matched = sample_count = trace_count = faults = 0;
    ReleaseSRWLockExclusive(&lock);
}

#ifdef CE_AI_DISTRICT_SCORES_TEST
/* Isolated native tests, excluded from normal CE builds. Synthetic buffers and
 * stub originals exercise contracts without loading Civ. They cannot validate
 * real ABI/worker timing, HKS lifetime, mod coexistence or context destruction. */
#include <stdarg.h>
static int test_base, test_bump_epoch, test_integer;
static char test_json[32768], test_previous[32768];
static int test_stack_checks, test_concat_fault;
static int *__cdecl test_net(int *out, unsigned d, int *t, int r, int h, void *p, void *c) {
    (void)d;
    (void)t;
    (void)r;
    (void)h;
    (void)p;
    (void)c;
    *out = test_base;
    if (test_bump_epoch)
        InterlockedIncrement(&generation);
    return out;
}
static void __cdecl test_cache(void *object) {
    (void)object;
}
static int __cdecl test_best(void *object, int *coordinates) {
    (void)object;
    coordinates[0] = 0;
    coordinates[1] = 3;
    return 8;
}
static int *__cdecl test_present(void *object, int *out, int district, void *plot) {
    (void)object;
    (void)district;
    (void)plot;
    *out = 3579;
    return out;
}
static int __cdecl test_check(void *L, int index) {
    return ((int *)L)[index - 1];
}
static void __cdecl test_push_integer(void *L, int value) {
    (void)L;
    test_integer = value;
}
static const char *__cdecl test_push_string(void *L, const char *format, ...) {
    (void)L;
    strcpy(test_previous, test_json);
    va_list a;
    va_start(a, format);
    vsnprintf(test_json, 512, format, a);
    va_end(a);
    return test_json;
}
static void __cdecl test_concat(void *L, int count) {
    (void)L;
    if (count != 2)
        test_concat_fault = 1;
    strcat(test_previous, test_json);
    strcpy(test_json, test_previous);
}
static void __cdecl test_stack(void *L, int count, const char *message) {
    (void)L;
    if (count != 2 || !message)
        test_concat_fault = 1;
    test_stack_checks++;
}
__declspec(dllexport) int CE_AIDistrictScoresSelfTest(void) {
    int n = 0, v, out = 0, xy[2], params[3];
#define CHECK(c) do { n++; if(!(c)) { printf("district native test %d failed: %s\n",n,#c);return -n; } } while(0)
    CHECK(adjusted(1000, 256, &v) && v == 1256);
    CHECK(adjusted(1000, -256, &v) && v == 744);
    CHECK(!adjusted(0, 256, &v) && v == 0);
    CHECK(!adjusted(INVALID_SCORE, 256, &v) && v == INVALID_SCORE);
    CHECK(!adjusted(INT_MAX, 256, &v) && v == INT_MAX);
    CHECK(adjusted(-1048500, -256, &v) && v == -1048576);
    CHECK(!adjusted(-1048577, -256, &v) && v == -1048577);
    _Alignas(8) unsigned char object[0x500] = {0}, city[0xe0] = {0}, row[0x78] = {0}, plot[0x30] = {0};
    int scores[10] = {0};
    *(void **)(object + 0x90) = city;
    *(void **)(object + 0x4c8) = row;
    *(unsigned *)(object + 0x4d8) = 1;
    *(int *)(city + 0xd8) = 1;
    *(int *)(city + 0xa8) = 7;
    *(short *)(plot + 0x28) = 0;
    *(short *)(plot + 0x2a) = 3;
    *(int *)(row + 8) = 0;
    *(int *)(row + 12) = 3;
    *(short *)(row + 0x70) = 1;
    *(int *)(row + 0x74) = 7;
    row[0x55] = row[0x5a] = 1;
    *(void **)(row + 0x20) = scores;
    scores[8] = 2468;
    memset(&policy, 0, sizeof policy);
    policy.version = 4;
    policy.width = policy.height = 10;
    policy.districts = 10;
    policy.count = 2;
    policy.allowed[1] = 1;
    policy.entries[0] = (Entry){1, 7, 8, 30, -65536};
    policy.entries[1] = (Entry){1, 7, 9, 31, 1048576};
    generation = 10;
    enabled = 1;
    faults = changed = matched = sample_count = trace_count = active = 0;
    original_net = test_net;
    original_cache = test_cache;
    original_best = test_best;
    original_present = test_present;
    original_base = 0;
    test_base = 5000;
    CHECK(bridge_net(&out, 8, NULL, 0, 0, plot, city, row, (void *)0x3e30be) == &out && out == -60536);
    CHECK(changed == 1 && trace_count == 1 && trace_records[0].kind == 0 && trace_records[0].reason == 0);
    CHECK(trace_records[0].generation == 10 && trace_records[0].version == 4 && trace_records[0].player == 1 && active == 0);
    test_base = 0;
    bridge_net(&out, 8, NULL, 0, 0, plot, city, row, (void *)0x3e30be);
    CHECK(out == 0 && changed == 1 && trace_count == 2 && trace_records[1].reason == 2);
    test_base = INVALID_SCORE;
    bridge_net(&out, 8, NULL, 0, 0, plot, city, row, (void *)0x3e30be);
    CHECK(out == INVALID_SCORE && trace_records[2].reason == 2);
    test_base = -1000000;
    bridge_net(&out, 8, NULL, 0, 0, plot, city, row, (void *)0x3e30be);
    CHECK(out == -1048576 && trace_records[3].reason == 0);
    *(short *)(plot + 0x28) = 1;
    *(int *)(row + 8) = 1;
    test_base = 100;
    bridge_net(&out, 9, NULL, 0, 0, plot, city, row, (void *)0x3e30be);
    CHECK(out == 1048676 && trace_records[4].plot == 31 && trace_records[4].district == 9);
    test_base = INT_MAX;
    bridge_net(&out, 9, NULL, 0, 0, plot, city, row, (void *)0x3e30be);
    CHECK(out == INT_MAX && trace_records[5].reason == 3 && trace_count == 6);
    int before = trace_count;
    test_base = 100;
    test_bump_epoch = 1;
    bridge_net(&out, 9, NULL, 0, 0, plot, city, row, (void *)0x3e30be);
    test_bump_epoch = 0;
    CHECK(out == 100 && trace_count == before && active == 0);
    *(int *)(city + 0xd8) = 0;
    *(short *)(row + 0x70) = 0;
    bridge_net(&out, 9, NULL, 0, 0, plot, city, row, (void *)0x3e30be);
    CHECK(out == 100 && trace_count == before);
    *(int *)(city + 0xd8) = 1;
    *(short *)(row + 0x70) = 1;
    *(short *)(plot + 0x28) = 0;
    *(int *)(row + 8) = 0;
    hook_cache(object);
    CHECK(trace_count == before + 1 && trace_records[before].kind == 1 && trace_records[before].base == 2468);
    CHECK(hook_best(object, xy) == 8 && xy[0] == 0 && xy[1] == 3);
    CHECK(trace_count == before + 2 && trace_records[before + 1].kind == 2 && trace_records[before + 1].district == 8);
    CHECK(hook_present(object, &out, 8, plot) == &out && out == 3579);
    CHECK(trace_count == before + 3 && trace_records[before + 2].kind == 3 && trace_records[before + 2].base == 3579);
    before = trace_count;
    hook_present(object, &out, 8, NULL);
    CHECK(trace_count == before && active == 0);
    check_integer = test_check;
    push_string = test_push_string;
    push_integer = test_push_integer;
    concat_strings = test_concat;
    check_stack = test_stack;
    char long_json[1501];
    memset(long_json, 'x', 1500);
    long_json[0] = '{';
    long_json[1] = '%';
    long_json[1499] = '}';
    long_json[1500] = 0;
    push_json(NULL, long_json, 1500);
    CHECK(strlen(test_json) == 1500 && !memcmp(test_json, long_json, 1500));
    CHECK(test_stack_checks == 1 && !test_concat_fault);
    push_json(NULL, "", 0);
    CHECK(test_json[0] == 0);
    params[0] = 0;
    params[1] = 2;
    params[2] = generation;
    CHECK(l_trace_read(params) == 1 && strstr(test_json, "\"stale\":false") && strstr(test_json, "\"next\":2"));
    CHECK(strstr(test_json, "\"k\":\"net\"") && strstr(test_json, "\"result\":-60536"));
    params[0] = 2;
    params[1] = 1;
    CHECK(l_trace_read(params) == 1 && strstr(test_json, "\"offset\":2") && strstr(test_json, "\"next\":3"));
    params[2] = generation - 1;
    CHECK(l_trace_read(params) == 1 && strstr(test_json, "\"stale\":true") && strstr(test_json, "\"records\":[]"));
    params[1] = 65;
    CHECK(l_trace_read(params) == 1 && strstr(test_json, "invalid_trace_page"));
    CHECK(l_trace_count(params) == 1 && test_integer == trace_count);
    for (int i = 0; i < 64; i++)
        samples[i] = (Sample){{1, 7, 8, i, 256}, 1000, 1256};
    sample_count = 64;
    CHECK(l_status(NULL) == 1 && strlen(test_json) > 512 && strstr(test_json, "\"trace_count\":") && test_json[strlen(test_json) - 1] == '}');
    for (int i = 0; i < 64; i++)
        trace_records[i] = trace_records[0];
    trace_count = 64;
    params[0] = 0;
    params[1] = 64;
    params[2] = generation;
    CHECK(l_trace_read(params) == 1 && strlen(test_json) > 512 && strstr(test_json, "\"next\":64") && test_json[strlen(test_json) - 1] == '}');
    trace_count = TRACE_CAPACITY;
    test_base = 100;
    bridge_net(&out, 8, NULL, 0, 0, plot, city, row, (void *)0x3e30be);
    CHECK(out == 100 && enabled == 0 && faults == 1 && active == 0);
    reset_policy();
    CHECK(enabled == 0 && policy.version == 0 && policy.count == 0 && trace_count == 0 && faults == 0 && active == 0);
#undef CHECK
    return n;
}
#endif

/* CE owns process initialization, gameplay Lua registration and context
 * retirement. Required exports are resolved independently of CE's HKS wrappers
 * so this capability fails closed if its complete JSON transport is unavailable. */
int CE_AIDistrictScoresInitialize(HMODULE core) {
    wchar_t path[32768];
    if (!core || !GetModuleFileNameW(core, path, 32768) || !verify_file(path))
        return 0;
    original = core;
    original_base = (uintptr_t)core;
    return install_hooks();
}
void CE_AIDistrictScoresRegisterLua(void *L) {
    if (push_closure && set_field && push_integer && check_integer && push_string && concat_strings && check_stack)
        register_bindings(L);
}
void CE_AIDistrictScoresReset(void) {
    reset_policy();
}
