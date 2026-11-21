
// A published preference for one city/building. Behaviours can nominate a
// building for prerequisite preparation. The queue can also use this selector
// to pick an immediately constructible fallback. Each caller has its own
// preference and receipts because the two results go to different decisions.
extern "C" void AIBuildingNominationEntry();
bool nomination_ready{}, queue_nomination_ready{};
std::atomic<bool> nominations_published{};
long nomination_changed{}, queue_nomination_changed{}, nomination_faults{};
std::array<int, 5> first_nomination_change{}, first_queue_nomination_change{};

auto nomination_key(const BuildingNomination& entry) {
    return std::tie(entry.building, entry.player, entry.city);
}

void finish_nominations(Policy& staged) {
    const auto first = staged.nominations.begin();
    const auto end = first + staged.nominationCount;
    std::sort(first, end, [](const auto& a, const auto& b) { return nomination_key(a) < nomination_key(b); });
    auto output = first;
    for (auto entry = first; entry != end; ++entry) {
        // Keep the greatest value for each consumer independently. An
        // ordinary-only preference cannot be promoted into the queue by
        // a second offer for the same city/building.
        if (output != first && nomination_key(*(output - 1)) == nomination_key(*entry)) {
            auto& combined = *(output - 1);
            combined.deltaQ8 = (std::max)(combined.deltaQ8, entry->deltaQ8);
            combined.queueDeltaQ8 = (std::max)(combined.queueDeltaQ8, entry->queueDeltaQ8);
        } else {
            *output++ = *entry;
        }
    }
    staged.nominationCount = static_cast<int>(output - first);
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
    if (!nominations_published.load(std::memory_order_relaxed) || !enabled.load() || building < 0)
        return score;
    const bool ordinary = caller == Runtime::GetGameCoreGlobalAt<const void*>(OrdinaryBuildingNominationReturnRva);
    const bool queue = !ordinary &&
        (caller == Runtime::GetGameCoreGlobalAt<const void*>(QueueBuildingSelectionReturnRva) ||
         caller == Runtime::GetGameCoreGlobalAt<const void*>(QueueFilteredBuildingSelectionReturnRva));
    if (!ordinary && !queue)
        return score;

    std::unique_lock guard(policyMutex);
    if (!enabled || !nomination_ready || (queue && !queue_nomination_ready) ||
        nomination_faults || !policy.nominationCount)
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
    const int delta = queue ? entry->queueDeltaQ8 : entry->deltaQ8;
    if (!delta)
        return score;

    auto& comparisons = queue ? entry->queueComparisons : entry->comparisons;
    auto& writes = queue ? entry->queueWrites : entry->writes;
    auto& changed = queue ? queue_nomination_changed : nomination_changed;
    auto& firstChange = queue ? first_queue_nomination_change : first_nomination_change;
    ++comparisons;
    const int next = adjusted_nomination_score(score, delta);
    if (next != score) {
        ++writes;
        if (changed++ == 0)
            firstChange = {key.player, key.city, key.building, score, next};
    }
    return next;
}

bool calls_building_selector(const unsigned char* base, uintptr_t callRva, uintptr_t returnRva) {
    const auto* caller = base + callRva;
    return *caller == X64CallRel32 && static_cast<intptr_t>(returnRva) + read_field<int32_t>(caller, 1) ==
        static_cast<intptr_t>(GetBestBuildingRva);
}

bool install_nomination_hook(const unsigned char* base) {
    queue_nomination_ready = false;
    if (!calls_building_selector(base, OrdinaryBuildingNominationCallRva, OrdinaryBuildingNominationReturnRva) ||
        std::memcmp(base + BuildingNominationComparisonRva, BuildingNominationCompare, sizeof BuildingNominationCompare))
        return false;
    if (!Runtime::InstallInstructionCall(BuildingNominationRestoreFlagRva,
        BuildingNominationRestoreFlag, &AIBuildingNominationEntry))
        return false;

    // A change to a queue caller disables that optional consumer. Ordinary
    // nomination remains available on the already qualified selector.
    queue_nomination_ready = calls_building_selector(base, QueueBuildingSelectionCallRva, QueueBuildingSelectionReturnRva) &&
        calls_building_selector(base, QueueFilteredBuildingSelectionCallRva, QueueFilteredBuildingSelectionReturnRva);
    return true;
}

int stage_nomination(lua_State* L, const BuildingNomination& entry) {
    bool ok;
    {
        std::unique_lock guard(policyMutex);
        ok = nomination_ready && !nomination_faults && staging_valid && staging.nominationCount < MaxEntries &&
            entry.player >= 0 && entry.player < MaxPlayers && staging.allowed[entry.player] &&
            entry.city >= 0 && entry.building >= 0 && entry.deltaQ8 >= 0 && entry.deltaQ8 <= MaxNominationBidQ8 &&
            entry.queueDeltaQ8 >= 0 && entry.queueDeltaQ8 <= MaxNominationBidQ8 &&
            (entry.deltaQ8 > 0 || entry.queueDeltaQ8 > 0) && (!entry.queueDeltaQ8 || queue_nomination_ready);
        if (ok)
            staging.nominations[staging.nominationCount++] = entry;
    }
    return integer_result(L, ok);
}

int l_nomination_bid(lua_State* L) {
    return stage_nomination(L, {hks::checkinteger(L, 1), hks::checkinteger(L, 2),
        hks::checkinteger(L, 3), hks::checkinteger(L, 4)});
}

int l_nomination_queue_bid(lua_State* L) {
    return stage_nomination(L, {hks::checkinteger(L, 1), hks::checkinteger(L, 2),
        hks::checkinteger(L, 3), hks::checkinteger(L, 4), hks::checkinteger(L, 5)});
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
    char output[224];
    const int length = snprintf(output, sizeof output,
        "{\"staged\":%d,\"forecastReady\":%d,\"forecasts\":%d,\"comparisons\":%d,\"writes\":%d,"
        "\"queueBidQ8\":%d,\"queueComparisons\":%d,\"queueWrites\":%d}",
        staged, static_cast<int>(ordinary_forecast_ready && wonder_ready),
        receipt.forecasts, receipt.comparisons, receipt.writes, receipt.queueDeltaQ8,
        receipt.queueComparisons, receipt.queueWrites);
    if (length > 0 && length < static_cast<int>(sizeof output))
        push_json(L, output, static_cast<size_t>(length));
    else
        push_json(L, "{}", 2);
    return 1;
}

int l_nomination_status(lua_State* L) {
    char output[416];
    int length;
    {
        std::shared_lock guard(policyMutex);
        const auto& a = first_nomination_change;
        const auto& q = first_queue_nomination_change;
        length = snprintf(output, sizeof output,
            "{\"ready\":%d,\"entries\":%d,\"maxBidQ8\":%d,\"changed\":%ld,\"faults\":%ld,\"first\":[%d,%d,%d,%d,%d],"
            "\"queueReady\":%d,\"queueChanged\":%ld,\"firstQueue\":[%d,%d,%d,%d,%d]}",
            static_cast<int>(nomination_ready), policy.nominationCount, MaxNominationBidQ8,
            nomination_changed, nomination_faults, a[0], a[1], a[2], a[3], a[4],
            static_cast<int>(queue_nomination_ready), queue_nomination_changed, q[0], q[1], q[2], q[3], q[4]);
    }
    if (length > 0 && length < static_cast<int>(sizeof output))
        push_json(L, output, static_cast<size_t>(length));
    else
        push_json(L, "{}", 2);
    return 1;
}
