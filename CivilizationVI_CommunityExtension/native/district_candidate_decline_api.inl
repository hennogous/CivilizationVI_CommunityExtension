/* Gameplay staging belongs to the same Begin/Allow/Commit transaction as
 * scores. This authorizes only a transient per-cell selector admission mask;
 * it issues no production, purchase, cancellation or retained-plan command.
 * Parse HKS arguments before locking (validation can longjmp). Missing/stale
 * layouts are fail-open and exact receipts separate staging from consumption.
 */
int l_decline_stage(lua_State* L) {
    const DistrictDecline scope{hks::checkinteger(L, 1), hks::checkinteger(L, 2),
        hks::checkinteger(L, 3), hks::checkinteger(L, 4)};
    bool ok;
    {
        std::unique_lock guard(policyMutex);
        ok = staging_valid && hooks_ready && !faults && staging.declineCount < MaxEntries &&
            scope.player >= 0 && scope.player < MaxPlayers && staging.allowed[scope.player] &&
            scope.city >= 0 && scope.district > 0 && scope.district < staging.districts &&
            scope.plot >= 0 && scope.plot < staging.width * staging.height;
        if (ok) staging.declines[staging.declineCount++] = scope;
        else staging_valid = false;
    }
    return integer_result(L, ok);
}
int l_decline_status(lua_State* L) {
    char output[320]; int length;
    {
        std::shared_lock guard(policyMutex);
        long considered = 0, masked = 0, restored = 0, rejected = 0;
        for (int i = 0; i < policy.declineCount; ++i) {
            considered += policy.declines[i].considered; masked += policy.declines[i].masked;
            restored += policy.declines[i].restored; rejected += policy.declines[i].rejected;
        }
        length = snprintf(output, sizeof output,
            "{\"abi\":1,\"ready\":%d,\"version\":%d,\"scopes\":%d,\"capacity\":%d,\"considered\":%ld,\"masked\":%ld,\"restored\":%ld,\"rejected\":%ld,\"faults\":%ld}",
            static_cast<int>(hooks_ready && !faults), policy.version, policy.declineCount, MaxEntries,
            considered, masked, restored, rejected, faults);
    }
    if (length > 0 && length < static_cast<int>(sizeof output)) push_json(L, output, static_cast<size_t>(length));
    else push_json(L, "{}", 2);
    return 1;
}
int l_decline_receipt(lua_State* L) {
    const int version = hks::checkinteger(L, 1);
    const DistrictDecline key{hks::checkinteger(L, 2), hks::checkinteger(L, 3),
        hks::checkinteger(L, 4), hks::checkinteger(L, 5)};
    DistrictDecline snapshot{}; bool staged = false;
    {
        std::shared_lock guard(policyMutex);
        if (version == policy.version) {
            const auto end = policy.declines.begin() + policy.declineCount;
            const auto found = std::lower_bound(policy.declines.begin(), end, key,
                [](const auto& a, const auto& b) { return decline_key(a) < decline_key(b); });
            staged = found != end && decline_key(*found) == decline_key(key);
            if (staged) snapshot = *found;
        }
    }
    char output[256];
    const int length = snprintf(output, sizeof output,
        "{\"staged\":%d,\"considered\":%ld,\"masked\":%ld,\"restored\":%ld,\"rejected\":%ld,\"originalQ8\":%d}",
        static_cast<int>(staged), snapshot.considered, snapshot.masked, snapshot.restored,
        snapshot.rejected, snapshot.lastOriginal);
    if (length > 0 && length < static_cast<int>(sizeof output)) push_json(L, output, static_cast<size_t>(length));
    else push_json(L, "{}", 2);
    return 1;
}
