
// Generic preference for one building in one city. This reads a published
// packet only at the ordinary selector's effective-score comparison.
extern "C" void AIBuildingNominationEntry();
bool nomination_ready{};
std::atomic<bool> nominations_published{};
long nomination_changed{}, nomination_faults{};
std::array<int, 5> first_nomination_change{};

auto nomination_key(const BuildingNomination& entry) {
    return std::tie(entry.building, entry.player, entry.city);
}

void finish_nominations(Policy& staged) {
    const auto first = staged.nominations.begin();
    const auto end = first + staged.nominationCount;
    std::sort(first, end, [](const auto& a, const auto& b) {
        if (nomination_key(a) != nomination_key(b))
            return nomination_key(a) < nomination_key(b);
        return a.deltaQ8 > b.deltaQ8;
    });
    // A shared producer can advance several chains. CSC owns that valuation.
    // Identical entries keep the largest published preference without stacking.
    staged.nominationCount = static_cast<int>(std::unique(first, end, [](const auto& a, const auto& b) {
        return nomination_key(a) == nomination_key(b);
    }) - first);
}

// All callers hold policyMutex. Identity remains scoped to a city and its
// current owner. Readback fields are carried by the same staged record.
BuildingNomination* find_nomination(const BuildingNomination& key) {
    const auto end = policy.nominations.begin() + policy.nominationCount;
    const auto at = std::lower_bound(policy.nominations.begin(), end, key,
        [](const auto& a, const auto& b) { return nomination_key(a) < nomination_key(b); });
    return at != end && nomination_key(*at) == nomination_key(key) ? &*at : nullptr;
}

void observe_nomination_forecast(void* city, unsigned building) {
    if (!nominations_published.load(std::memory_order_relaxed) || !enabled.load() || building > INT_MAX)
        return;
    std::unique_lock guard(policyMutex);
    if (!enabled || !readable(city, CityOwnerOffset + sizeof(int), false))
        return;
    const auto* bytes = static_cast<const unsigned char*>(city);
    const BuildingNomination key{read_field<int>(bytes, CityOwnerOffset),
        read_field<int>(bytes, CityIdOffset), static_cast<int>(building), 0};
    if (auto* entry = find_nomination(key))
        ++entry->forecasts;
}

int adjusted_nomination_score(int score, int deltaQ8) {
    if (score <= 0 || deltaQ8 <= 0 || deltaQ8 > MaxNominationBidQ8 || score > INT_MAX - deltaQ8)
        return score;
    return score + deltaQ8;
}

// The adapter replaces a single instruction after the selector's prerequisite
// branches. R15 is CityBuild, R12D is the building index, and R14D is its
// effective Q8. It returns to the original CMP, leaving every cache unchanged.
extern "C" int AIBuildingNominationScore(const void* cityBuild, int building, int score,
    const void* caller) {
    if (!nominations_published.load(std::memory_order_relaxed) || !enabled.load() ||
        caller != Runtime::GetGameCoreGlobalAt<const void*>(OrdinaryBuildingNominationReturnRva)
        || building < 0)
        return score;

    std::unique_lock guard(policyMutex);
    if (!enabled || !nomination_ready || nomination_faults || !policy.nominationCount)
        return score;
    const auto end = policy.nominations.begin() + policy.nominationCount;
    const auto first = std::lower_bound(policy.nominations.begin(), end, building,
        [](const auto& entry, int index) { return entry.building < index; });
    if (first == end || first->building != building)
        return score;

    if (!readable(cityBuild, CityBuildCityOffset + sizeof(void*), false)) {
        ++nomination_faults;
        return score;
    }
    const auto* city = read_field<const unsigned char*>(static_cast<const unsigned char*>(cityBuild), CityBuildCityOffset);
    if (!readable(city, CityOwnerOffset + sizeof(int), false)) {
        ++nomination_faults;
        return score;
    }
    const int player = read_field<int>(city, CityOwnerOffset);
    if (player < 0 || player >= MaxPlayers || !policy.allowed[player])
        return score;

    const BuildingNomination key{player, read_field<int>(city, CityIdOffset), building, 0};
    auto* entry = find_nomination(key);
    if (!entry)
        return score;

    ++entry->comparisons;
    const int next = adjusted_nomination_score(score, entry->deltaQ8);
    if (next != score) {
        ++entry->writes;
        if (nomination_changed++ == 0)
            first_nomination_change = {key.player, key.city, key.building, score, next};
    }
    return next;
}

bool install_nomination_hook(const unsigned char* base) {
    const auto* caller = base + OrdinaryBuildingNominationCallRva;
    if (*caller != X64CallRel32 || read_field<int32_t>(caller, 1) !=
        static_cast<int32_t>(GetBestBuildingRva - OrdinaryBuildingNominationReturnRva))
        return false;
    if (std::memcmp(base + BuildingNominationComparisonRva, BuildingNominationCompare, sizeof BuildingNominationCompare))
        return false;
    return Runtime::InstallInstructionCall(BuildingNominationRestoreFlagRva,
        BuildingNominationRestoreFlag, &AIBuildingNominationEntry);
}

int l_nomination_bid(lua_State* L) {
    const BuildingNomination entry{hks::checkinteger(L, 1), hks::checkinteger(L, 2),
        hks::checkinteger(L, 3), hks::checkinteger(L, 4)};
    bool ok;
    {
        std::unique_lock guard(policyMutex);
        ok = nomination_ready && !nomination_faults && staging_valid && staging.nominationCount < MaxEntries &&
            entry.player >= 0 && entry.player < MaxPlayers && staging.allowed[entry.player] &&
            entry.city >= 0 && entry.building >= 0 && entry.deltaQ8 > 0 && entry.deltaQ8 <= MaxNominationBidQ8;
        if (ok)
            staging.nominations[staging.nominationCount++] = entry;
    }
    return integer_result(L, ok);
}

int l_nomination_receipt(lua_State* L) {
    const int version = hks::checkinteger(L, 1);
    const BuildingNomination key{hks::checkinteger(L, 2), hks::checkinteger(L, 3),
        hks::checkinteger(L, 4), 0};
    int staged = 0;
    BuildingNomination receipt{};
    {
        std::shared_lock guard(policyMutex);
        if (version == policy.version) {
            if (const auto* entry = find_nomination(key)) {
                receipt = *entry;
                staged = 1;
            }
        }
    }
    char output[160];
    const int length = snprintf(output, sizeof output,
        "{\"staged\":%d,\"forecastReady\":%d,\"forecasts\":%d,\"comparisons\":%d,\"writes\":%d}",
        staged, static_cast<int>(ordinary_forecast_ready && wonder_ready),
        receipt.forecasts, receipt.comparisons, receipt.writes);
    if (length > 0 && length < static_cast<int>(sizeof output))
        push_json(L, output, static_cast<size_t>(length));
    else
        push_json(L, "{}", 2);
    return 1;
}

int l_nomination_status(lua_State* L) {
    char output[320];
    int length;
    {
        std::shared_lock guard(policyMutex);
        const auto& a = first_nomination_change;
        length = snprintf(output, sizeof output,
            "{\"ready\":%d,\"entries\":%d,\"maxBidQ8\":%d,\"changed\":%ld,\"faults\":%ld,\"first\":[%d,%d,%d,%d,%d]}",
            static_cast<int>(nomination_ready), policy.nominationCount, MaxNominationBidQ8,
            nomination_changed, nomination_faults, a[0], a[1], a[2], a[3], a[4]);
    }
    if (length > 0 && length < static_cast<int>(sizeof output))
        push_json(L, output, static_cast<size_t>(length));
    else
        push_json(L, "{}", 2);
    return 1;
}
