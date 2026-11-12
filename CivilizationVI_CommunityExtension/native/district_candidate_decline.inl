/* A typed alternative to the whole-plot SupplierRowMask.
 *
 * ABI/layout scope is the already fingerprinted GetBestDistrict entry and
 * bounded CityBuild matrix in target_identity.h (XP2 reference324c51e9,
 * hotfix092abfbb matching text). No additional address, detour or Lua callback.
 * GetBestDistrict reads scores/reservations without reallocating this matrix.
 * Restore every borrowed cell before cache readback or district preparation.
 *
 * This is not a forecast correction. Stock selection accepts zero/negative
 * values at/above InvalidScore with a low reserved threshold. Decline must be
 * strictly BELOW the initial native best, temporarily.
 * Reservation handles are deliberately untouched: reserved cells can still
 * win above their reservation priority and must not escape a typed decline.
 */
// GetBestDistrict starts its best at InvalidScore and admits a cell when
// reservationPriority < cell && best <= cell. Equal-to-floor is insufficient
// for reserved rows with a lower priority. Below-floor cannot win either test.
constexpr int DeclinedScore = InvalidScore - 1;

auto decline_key(const DistrictDecline& scope) {
    return std::tie(scope.player, scope.city, scope.district, scope.plot);
}
void finish_declines(Policy& value) {
    const auto first = value.declines.begin();
    const auto end = first + value.declineCount;
    std::sort(first, end, [](const auto& a, const auto& b) { return decline_key(a) < decline_key(b); });
    value.declineCount = static_cast<int>(std::unique(first, end, [](const auto& a, const auto& b) {
        return decline_key(a) == decline_key(b);
    }) - first);
}
class DistrictCandidateMask {
public:
    DistrictCandidateMask(void* object, const Observation& observation) : epoch(observation.epoch) {
        if (!observation.accepted)
            return;
        std::unique_lock guard(policyMutex);
        if (!enabled || generation != epoch || !policy.declineCount)
            return;
        const DistrictDecline cityKey{observation.key.player, observation.key.city, 0, 0};
        const auto end = policy.declines.begin() + policy.declineCount;
        const auto first = std::lower_bound(policy.declines.begin(), end, cityKey,
            [](const auto& a, const auto& b) { return decline_key(a) < decline_key(b); });
        const auto last = std::find_if(first, end, [&](const auto& scope) {
            return scope.player != cityKey.player || scope.city != cityKey.city;
        });
        if (first == last)
            return;
        const auto cityCount = static_cast<size_t>(last - first);
        const auto matrix = read_matrix(object, observation.key);
        if (!matrix.rows)
            return; // Unsupported/stale layout: ordinary selection, no write.
        // At most one borrowed i32 per staged key; no global scan or policy
        // pointer survives the original game call. Allocate only for this city.
        try {
            saved.reserve(cityCount);
        } catch (const std::bad_alloc&) {
            // Never unwind a C++ allocation exception through game frames.
            // No cell has changed; retire this capability and select normally.
            native_fault();
            return;
        }
        for (auto item = first; item != last; ++item) {
            const int key = static_cast<int>(item - policy.declines.begin());
            auto& scope = *item;
            ++scope.considered; // Consultation is not a selected action.
            for (unsigned index = 1; index < matrix.rowCount; ++index) {
                const auto* row = matrix.rows + index * PlotAnalysisRowSize;
                const int x = read_field<int>(row, RowXOffset), y = read_field<int>(row, RowYOffset);
                if (x < 0 || y < 0 || x >= policy.width || y >= policy.height ||
                    y * policy.width + x != scope.plot)
                    continue;
                // Native analysis tags even frontier rows with the analysis
                // owner/city. Do not confuse that tag with plot ownership.
                if (!row[RowDistrictEligibleOffset] || !(row[RowOwnedOffset] || row[RowFrontierOffset]) ||
                    read_field<int16_t>(row, RowOwnerOffset) != scope.player ||
                    read_field<int>(row, RowCityIdOffset) != scope.city)
                    break;
                auto* rowScores = matrix.scores + index * policy.districts;
                auto* cell = rowScores + scope.district;
                if (read_field<int*>(row, RowDistrictScoresOffset) != rowScores ||
                    !readable(cell, sizeof(*cell), true)) {
                    ++scope.rejected;
                    break;
                }
                // Preserve native invalid/protected values. A nested mask
                // observes the floor and cannot prematurely undo its parent.
                if (*cell >= InvalidScore && *cell <= MaxScore)
                    saved.push_back({cell, *cell, key});
                break;
            }
        }
        // Journal capacity failure must not create a partly masked selector.
        if (saved.size() > static_cast<size_t>(TraceCapacity - trace_count)) {
            native_fault();
            saved.clear();
            return;
        }
        for (const auto& item : saved) {
            auto& scope = policy.declines[item.key];
            record_trace(Decline, {scope.player, scope.city, scope.district, scope.plot, 0},
                scope.district, scope.plot, item.original, DeclinedScore, Applied, 1);
            *item.cell = DeclinedScore;
            ++scope.masked;
            scope.lastOriginal = item.original;
        }
    }
    ~DistrictCandidateMask() {
        // Original selector does not relocate/free the matrix. Restoration is
        // unconditional even if a concurrent publication retired this scope.
        for (const auto& item : saved)
            *item.cell = item.original;
        if (saved.empty())
            return;
        std::unique_lock guard(policyMutex);
        if (generation == epoch)
            for (const auto& item : saved)
                ++policy.declines[item.key].restored;
    }
    DistrictCandidateMask(const DistrictCandidateMask&) = delete;
    DistrictCandidateMask& operator=(const DistrictCandidateMask&) = delete;
private:
    struct Saved { int* cell; int original; int key; };
    std::vector<Saved> saved;
    long epoch{};
};
