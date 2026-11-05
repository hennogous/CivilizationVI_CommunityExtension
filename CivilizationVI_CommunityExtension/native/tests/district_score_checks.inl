/* Isolated native tests, excluded from normal CE builds. Synthetic buffers and
 * stub originals exercise contracts without loading Civ. They cannot validate
 * real ABI/worker timing, HKS lifetime, mod coexistence or context destruction. */
int test_base, test_bump_epoch, test_integer;
char test_json[32768], test_previous[32768];
int test_stack_checks, test_concat_fault;
int* __cdecl test_net(int* out, unsigned d, int* t, int r, int h, void *p, void *c) {
    (void)d;
    (void)t;
    (void)r;
    (void)h;
    (void)p;
    (void)c;
    *out = test_base;
    if (test_bump_epoch)
        ++generation;
    return out;
}
int* cacheTestScore{};
int cacheTestFinal{};
int* cacheTestReservation{};
void __cdecl test_cache(void*) {
    if (cacheTestScore)
        *cacheTestScore = cacheTestFinal;
    if (cacheTestReservation)
        *cacheTestReservation = 1;
    if (test_bump_epoch)
        ++generation;
}
int __cdecl test_best(void* object, int* coordinates) {
    (void)object;
    coordinates[0] = 0;
    coordinates[1] = 3;
    return 8;
}
int* __cdecl test_present(void* object, int* out, int district, void* plot) {
    (void)object;
    (void)district;
    (void)plot;
    *out = 3579;
    return out;
}
int __cdecl test_check(lua_State* L, int index) {
    return reinterpret_cast<int*>(L)[index - 1];
}
void __cdecl test_push_integer(lua_State* L, int value) {
    (void)L;
    test_integer = value;
}
const char* __cdecl test_push_string(lua_State* L, const char* format, ...) {
    (void)L;
    strcpy_s(test_previous, test_json);
    va_list a;
    va_start(a, format);
    vsnprintf(test_json, 512, format, a);
    va_end(a);
    return test_json;
}
void __cdecl test_concat(lua_State* L, int count) {
    (void)L;
    if (count != 2)
        test_concat_fault = 1;
    strcat_s(test_previous, test_json);
    strcpy_s(test_json, test_previous);
}
void __cdecl test_stack(lua_State* L, int count, const char* message) {
    (void)L;
    if (count != 2 || !message)
        test_concat_fault = 1;
    test_stack_checks++;
}
template <typename Value>
void put_field(unsigned char* buffer, size_t offset, Value value) {
    std::memcpy(buffer + offset, &value, sizeof value);
}

int run_self_test() {
    int n = 0, v, out = 0, xy[2], params[3];
#define CHECK(c) do { n++; if(!(c)) { printf("district native test %d failed: %s\n",n,#c);return -n; } } while(0)
    CHECK(adjusted(1000, 256, &v) && v == 1256);
    CHECK(adjusted(1000, -256, &v) && v == 744);
    CHECK(!adjusted(0, 256, &v) && v == 0);
    CHECK(!adjusted(InvalidScore, 256, &v) && v == InvalidScore);
    CHECK(!adjusted(INT_MAX, 256, &v) && v == INT_MAX);
    CHECK(adjusted(-1048500, -256, &v) && v == MinimumScore);
    CHECK(!adjusted(-1048577, -256, &v) && v == -1048577);
    alignas(8) unsigned char object[DistrictMatrixLengthOffset + sizeof(unsigned)]{};
    alignas(8) unsigned char city[CityOwnerOffset + sizeof(int)]{};
    alignas(8) unsigned char rows[2 * PlotAnalysisRowSize]{};
    alignas(8) unsigned char plot[PlotYOffset + sizeof(int16_t)]{};
    auto* row = rows;
    int scores[10] = {0};
    put_field<void*>(object, CityBuildCityOffset, city);
    put_field<void*>(object, CityBuildRowsOffset, row);
    put_field<unsigned>(object, CityBuildRowCountOffset, 1);
    put_field<int>(city, CityOwnerOffset, 1);
    put_field<int>(city, CityIdOffset, 7);
    put_field<int16_t>(plot, PlotXOffset, 0);
    put_field<int16_t>(plot, PlotYOffset, 3);
    put_field<int>(row, RowXOffset, 0);
    put_field<int>(row, RowYOffset, 3);
    put_field<int16_t>(row, RowOwnerOffset, 1);
    put_field<int>(row, RowCityIdOffset, 7);
    row[RowDistrictEligibleOffset] = row[RowOwnedOffset] = 1;
    put_field<void*>(row, RowDistrictScoresOffset, scores);
    scores[8] = 2468;
    clear_policy(policy);
    policy.version = 4;
    policy.width = policy.height = 10;
    policy.districts = 10;
    policy.count = 2;
    policy.allowed[1] = 1;
    policy.entries[0] = Entry{1, 7, 8, 30, -65536};
    policy.entries[1] = Entry{1, 7, 9, 31, 1048576};
    generation = 10;
    enabled = true;
    faults = changed = matched = sample_count = trace_count = 0;
    active = 0;
    original_net = test_net;
    original_cache = test_cache;
    original_best = test_best;
    original_present = test_present;
    Runtime::GameCoreAddress = 0;
    auto* netCaller = Runtime::GetGameCoreGlobalAt<void*>(NetValueFromPlotAnalysisReturnRva);
    test_base = 5000;
    CHECK(AIDistrictScoreNet(&out, 8, NULL, 0, 0, plot, city, row, netCaller) == &out && out == -60536);
    CHECK(changed == 1 && trace_count == 1 && trace_records[0].kind == 0 && trace_records[0].reason == 0);
    CHECK(trace_records[0].generation == 10 && trace_records[0].version == 4 && trace_records[0].player == 1 && active == 0);
    test_base = 0;
    AIDistrictScoreNet(&out, 8, NULL, 0, 0, plot, city, row, netCaller);
    CHECK(out == 0 && changed == 1 && trace_count == 2 && trace_records[1].reason == 2);
    test_base = InvalidScore;
    AIDistrictScoreNet(&out, 8, NULL, 0, 0, plot, city, row, netCaller);
    CHECK(out == InvalidScore && trace_records[2].reason == 2);
    test_base = -1000000;
    AIDistrictScoreNet(&out, 8, NULL, 0, 0, plot, city, row, netCaller);
    CHECK(out == MinimumScore && trace_records[3].reason == 0);
    put_field<int16_t>(plot, PlotXOffset, 1);
    put_field<int>(row, RowXOffset, 1);
    test_base = 100;
    AIDistrictScoreNet(&out, 9, NULL, 0, 0, plot, city, row, netCaller);
    CHECK(out == 1048676 && trace_records[4].plot == 31 && trace_records[4].district == 9);
    test_base = INT_MAX;
    AIDistrictScoreNet(&out, 9, NULL, 0, 0, plot, city, row, netCaller);
    CHECK(out == INT_MAX && trace_records[5].reason == 3 && trace_count == 6);
    int before = trace_count;
    test_base = 100;
    test_bump_epoch = 1;
    AIDistrictScoreNet(&out, 9, NULL, 0, 0, plot, city, row, netCaller);
    test_bump_epoch = 0;
    CHECK(out == 100 && trace_count == before && active == 0);
    put_field<int>(city, CityOwnerOffset, 0);
    put_field<int16_t>(row, RowOwnerOffset, 0);
    AIDistrictScoreNet(&out, 9, NULL, 0, 0, plot, city, row, netCaller);
    CHECK(out == 100 && trace_count == before);
    put_field<int>(city, CityOwnerOffset, 1);
    put_field<int16_t>(row, RowOwnerOffset, 1);
    put_field<int16_t>(plot, PlotXOffset, 0);
    put_field<int>(row, RowXOffset, 0);
    hook_cache(object);
    CHECK(trace_count == before + 1 && trace_records[before].kind == 1 && trace_records[before].base == 2468);
    CHECK(hook_best(object, xy) == 8 && xy[0] == 0 && xy[1] == 3);
    CHECK(trace_count == before + 2 && trace_records[before + 1].kind == Best && trace_records[before + 1].district == 8);
    CHECK(hook_present(object, &out, 8, plot) == &out && out == 3579);
    CHECK(trace_count == before + 3 && trace_records[before + 2].kind == 3 && trace_records[before + 2].base == 3579);
    before = trace_count;
    hook_present(object, &out, 8, NULL);
    CHECK(trace_count == before && active == 0);
    hks::checkinteger = test_check;
    hks::pushfstring = test_push_string;
    hks::pushinteger = test_push_integer;
    hks::concat = test_concat;
    hks::checkstack = test_stack;
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
    CHECK(l_trace_read(reinterpret_cast<lua_State*>(params)) == 1 && strstr(test_json, "\"stale\":false") && strstr(test_json, "\"next\":2"));
    CHECK(strstr(test_json, "\"k\":\"net\"") && strstr(test_json, "\"result\":-60536"));
    params[0] = 2;
    params[1] = 1;
    CHECK(l_trace_read(reinterpret_cast<lua_State*>(params)) == 1 && strstr(test_json, "\"offset\":2") && strstr(test_json, "\"next\":3"));
    params[2] = generation - 1;
    CHECK(l_trace_read(reinterpret_cast<lua_State*>(params)) == 1 && strstr(test_json, "\"stale\":true") && strstr(test_json, "\"records\":[]"));
    params[1] = 65;
    CHECK(l_trace_read(reinterpret_cast<lua_State*>(params)) == 1 && strstr(test_json, "invalid_trace_page"));
    CHECK(l_trace_count(reinterpret_cast<lua_State*>(params)) == 1 && test_integer == trace_count);
    for (int i = 0; i < 64; i++)
        samples[i] = Sample{{1, 7, 8, i, 256}, 1000, 1256};
    sample_count = 64;
    CHECK(l_status(NULL) == 1 && strlen(test_json) > 512 && strstr(test_json, "\"trace_count\":") && test_json[strlen(test_json) - 1] == '}');
    for (int i = 0; i < 64; i++)
        trace_records[i] = trace_records[0];
    trace_count = 64;
    params[0] = 0;
    params[1] = 64;
    params[2] = generation;
    CHECK(l_trace_read(reinterpret_cast<lua_State*>(params)) == 1 && strlen(test_json) > 512 && strstr(test_json, "\"next\":64") && test_json[strlen(test_json) - 1] == '}');
    trace_count = TraceCapacity;
    test_base = 100;
    AIDistrictScoreNet(&out, 8, NULL, 0, 0, plot, city, row, netCaller);
    CHECK(out == 100 && enabled == 0 && faults == 1 && active == 0);
    reset_policy();
    CHECK(enabled == 0 && policy.version == 0 && policy.count == 0 && trace_count == 0 && faults == 0 && active == 0);

    const int lateBefore = 77534;
    const int lateNative = 104621;
    const int lateDelta = -4398;
    int matrix[20]{};
    int reservation = UnreservedPlot;
    auto* lateRow = rows + PlotAnalysisRowSize;
    std::memcpy(lateRow, rows, PlotAnalysisRowSize);
    put_field(lateRow, RowReservationOffset, &reservation);
    put_field(lateRow, RowDistrictScoresOffset, matrix + 10);
    put_field(rows, RowDistrictScoresOffset, matrix);
    put_field(rows, RowXOffset, 1); // city center; excluded from late writes
    put_field(object, CityBuildRowCountOffset, 2u);
    put_field(object, DistrictMatrixDataOffset, matrix + 0);
    put_field(object, DistrictMatrixLengthOffset, 20u);
    put_field(object, DistrictMatrixCapacityOffset, 20u);
    auto* lateCaller = Runtime::GetGameCoreGlobalAt<void*>(DistrictAggregationReturnRva);
    auto* score = &matrix[18];
    cacheTestScore = score;
    cacheTestFinal = lateNative;
    hooks_ready = true;
    auto asLua = [](int* arguments) { return reinterpret_cast<lua_State*>(arguments); };
    int begin[] = {1, 10, 10, 10};
    int allow[] = {1};
    int entry[] = {1, 7, 8, 30, lateBefore, lateNative, lateDelta};
    l_begin(asLua(begin));
    l_allow(asLua(allow));
    l_late_entry(asLua(entry));
    CHECK(test_integer == 1 && staging.hasLateEntries);
    CHECK(!stage_entry(Entry{1, 7, 8, 30, 256}, false) && !staging_valid);
    l_begin(asLua(begin));
    l_allow(asLua(allow));
    l_late_entry(asLua(entry));
    l_commit(nullptr);
    CHECK(test_integer == 1 && policy.hasLateEntries && enabled);
    test_base = lateBefore;
    AIDistrictScoreNet(&out, 8, nullptr, 0, 0, plot, city, rows, netCaller);
    CHECK(out == lateBefore && changed == 0);
    *score = lateBefore;
    calculate_district_values(object, lateCaller);
    CHECK(*score == lateNative + lateDelta && changed == 1 && active == 0);
    CHECK(trace_records[0].kind == Late && trace_records[0].base == lateNative &&
        trace_records[0].result == lateNative + lateDelta && trace_records[0].reason == Applied);
    CHECK(trace_records[1].kind == Cache && trace_records[1].base == lateNative + lateDelta);
    *score = lateBefore + 1;
    before = trace_count;
    calculate_district_values(object, lateCaller);
    CHECK(*score == lateNative && changed == 1 && trace_records[before].reason == LateInputMismatch);
    *score = lateBefore;
    cacheTestFinal = lateNative + 1;
    before = trace_count;
    calculate_district_values(object, lateCaller);
    CHECK(*score == lateNative + 1 && changed == 1 && trace_records[before].reason == LateFinalMismatch);
    cacheTestFinal = lateNative;
    *score = lateBefore;
    calculate_district_values(object, netCaller);
    CHECK(*score == lateNative && changed == 1);
    *score = lateBefore;
    reservation = 3;
    calculate_district_values(object, lateCaller);
    CHECK(*score == lateNative && changed == 1);
    *score = lateBefore;
    reservation = UnreservedPlot;
    cacheTestReservation = &reservation;
    calculate_district_values(object, lateCaller);
    CHECK(*score == lateNative && changed == 1 && reservation == 1);
    cacheTestReservation = nullptr;
    *score = lateBefore;
    reservation = UnreservedPlot;
    put_field(object, DistrictMatrixCapacityOffset, 19u);
    calculate_district_values(object, lateCaller);
    CHECK(*score == lateNative && changed == 1);
    put_field(object, DistrictMatrixCapacityOffset, 20u);
    *score = lateBefore;
    test_bump_epoch = true;
    calculate_district_values(object, lateCaller);
    CHECK(*score == lateNative && changed == 1 && active == 0);
    test_bump_epoch = false;
    cacheTestScore = nullptr;
#undef CHECK
    return n;
}
