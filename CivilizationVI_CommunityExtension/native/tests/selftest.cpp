// Isolated host for the native checks and the shared hook install failure paths.
#include "AIDistrictScores.h"
#include "Runtime.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
    enum class Failure { None, Create, Queue, Apply };
    Failure failAt;
    std::array<bool, 2> created, queued;
    unsigned removeCount, applyCount;
    // Distinct bodies keep the linker from coalescing the synthetic targets.
    int firstHook() { return 1; }
    int secondHook() { return 2; }
    int detour() { return 3; }

    int relayCalls;
    void relayCallback() { ++relayCalls; }

    bool check_instruction_call() {
        // sub rsp, 0x28; mov eax, 42; add rsp, 0x28; ret.
        // This synthetic function has the same stack alignment and room for
        // shadow space as the game at the inserted CALL.
        constexpr size_t site = 4;
        const std::array<unsigned char, 5> expected{0xb8, 42, 0, 0, 0};
        const std::array<unsigned char, 5> wrong{0xb8, 43, 0, 0, 0};
        const unsigned char code[]{0x48, 0x83, 0xec, 0x28, 0xb8, 42, 0, 0, 0,
            0x48, 0x83, 0xc4, 0x28, 0xc3};
        void* executable = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!executable)
            return false;
        std::memcpy(executable, code, sizeof code);
        DWORD previous{};
        if (!VirtualProtect(executable, 4096, PAGE_EXECUTE_READ, &previous) ||
            !FlushInstructionCache(GetCurrentProcess(), executable, sizeof code))
            return false;
        auto invoke = reinterpret_cast<int(*)()>(executable);
        Runtime::GameCore = LoadLibraryW(L"kernel32.dll");
        Runtime::GameCoreAddress = reinterpret_cast<uintptr_t>(executable);
        if (!Runtime::GameCore || invoke() != 42 ||
            Runtime::InstallInstructionCall(site, wrong, &relayCallback) ||
            !Runtime::InstallInstructionCall(site, expected, &relayCallback))
            return false;
        (void)invoke();
        Runtime::Destroy();
        const bool restored = relayCalls == 1 && invoke() == 42;
        return VirtualFree(executable, 0, MEM_RELEASE) && restored;
    }

    size_t targetIndex(LPVOID address) {
        if (address == reinterpret_cast<LPVOID>(&firstHook))
            return 0;
        if (address == reinterpret_cast<LPVOID>(&secondHook))
            return 1;
        std::abort();
    }

    bool check_install(Failure failure) {
        failAt = failure;
        created = queued = {};
        removeCount = applyCount = 0;
        int (*firstOriginal)(){};
        int (*secondOriginal)(){};
        const Runtime::HookSpec hooks[]{
            Runtime::DescribeHook("first", &firstHook, &detour, &firstOriginal),
            Runtime::DescribeHook("second", &secondHook, &detour, &secondOriginal),
        };
        const bool installed = Runtime::InstallHookGroup(hooks);
        if (failure == Failure::None)
            return installed && firstOriginal == &firstHook && secondOriginal == &secondHook &&
                removeCount == 0 && applyCount == 1;
        const unsigned expectedRemovals = failure == Failure::Create ? 1 : 2;
        const unsigned expectedApplyCalls = failure == Failure::Apply ? 1 : 0;
        return !installed && !created[0] && !created[1] && !firstOriginal && !secondOriginal &&
            removeCount == expectedRemovals && applyCount == expectedApplyCalls;
    }
}

extern "C" int* AIDistrictScoreNetEntry(int*, unsigned, int*, int, int, void*, void*) {
    std::abort();
}

extern "C" void AIBuildingNominationEntry() { std::abort(); }

MH_STATUS WINAPI MH_CreateHook(LPVOID target, LPVOID, LPVOID* original) {
    const auto index = targetIndex(target);
    if (index && failAt == Failure::Create)
        return MH_ERROR_MEMORY_ALLOC;
    created[index] = true;
    *original = target;
    return MH_OK;
}

MH_STATUS WINAPI MH_QueueEnableHook(LPVOID target) {
    const auto index = targetIndex(target);
    if (!created[0] || !created[1])
        std::abort();
    if (index && failAt == Failure::Queue)
        return MH_ERROR_NOT_CREATED;
    queued[index] = true;
    return MH_OK;
}

MH_STATUS WINAPI MH_ApplyQueued() {
    ++applyCount;
    if (!queued[0] || !queued[1])
        std::abort();
    return failAt == Failure::Apply ? MH_ERROR_MEMORY_PROTECT : MH_OK;
}

MH_STATUS WINAPI MH_RemoveHook(LPVOID target) {
    const auto index = targetIndex(target);
    if (!created[index])
        std::abort();
    created[index] = queued[index] = false;
    ++removeCount;
    return MH_OK;
}

MH_STATUS WINAPI MH_Initialize() { std::abort(); }
MH_STATUS WINAPI MH_DisableHook(LPVOID target) { return target == MH_ALL_HOOKS ? MH_OK : MH_ERROR_NOT_CREATED; }
MH_STATUS WINAPI MH_Uninitialize() { return MH_OK; }

const char* WINAPI MH_StatusToString(MH_STATUS) {
    return "synthetic hook failure";
}

int main() {
    const int checks = AI::DistrictScores::SelfTest();
    std::printf("DistrictScores checks=%d\n", checks);
    std::fflush(stdout);
    if (checks != 114)
        return 1;
    Log::Logger logger("HookGroupSelfTest");
    Runtime::MainLogger = &logger;
    for (auto failure : {Failure::None, Failure::Create, Failure::Queue, Failure::Apply}) {
        if (!check_install(failure))
            return 2;
    }
    std::puts("Runtime hook group checks=4");
    if (!check_instruction_call())
        return 3;
    std::puts("Runtime instruction call checked expected bytes, delivery and restoration");
    return 0;
}
