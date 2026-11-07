#include "Runtime.h"
#include "MinHook.h"
#include <iostream>
#include "HavokScript.h"
#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cstring>
#pragma comment(lib, "libMinHook.x64.lib")
#pragma comment(lib, "bcrypt.lib")

namespace Runtime {
	HMODULE GameCore;
	uintptr_t GameCoreAddress;
	DWORD GameProcessId;
	HANDLE GameProcess;
	HMODULE CivVIModHandle;

	Log::Logger* MainLogger;

	namespace Base {
		EXP_GetLogPath_t EXP_GetLogPath;

		namespace {
			// Windows Steam DX11. The hotfix moved executable code and data
			// while the XP2 code section stayed identical. See the CSC binary
			// comparison for the unwind, instruction and data-reference proof.
			constexpr ExecutableProfile profiles[] = {
				{"Steam DX11 reference",
				 "e7450823cc8e00468cff7b9d7b97c63140eae38ae1d774ba4efa437556c42d63",
				 "324c51e9ea3531758842e16c69e6cddbefbb226c5b675c3d6e60111646c2e98c",
				 0x6e260, 0x3552fa0, 0x88b330, 0x8948a0, 0x235f7c8,
				 {0x40,0x57,0x48,0x83,0xec,0x70,0xf2,0x0f,0x10,0x15,0x8a,0xce,0xf3,0x00,0x4c,0x8d,0x0d,0x1b,0x5a,0x25,0x01,0x48,0x8b,0xf9}},
				{"Steam DX11 November hotfix",
				 "d1a0cfd439e7451be515f47595cfa0b6b983f56d1faf4ea434a4ce3b21491ebe",
				 "092abfbb2176f8df04b5212f819dc269dd83aea866ff05aaf489904a1ff7c41f",
				 0x6d500, 0x3553fa0, 0x88b9b0, 0x895000, 0x23607c8,
				 {0x40,0x57,0x48,0x83,0xec,0x70,0xf2,0x0f,0x10,0x15,0xba,0xeb,0xf3,0x00,0x4c,0x8d,0x0d,0x7b,0x77,0x25,0x01,0x48,0x8b,0xf9}},
			};
			const ExecutableProfile* selectedProfile = nullptr;

			std::string ImageSha256(HMODULE module) {
				std::array<wchar_t, 32768> path;
				const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
				if (!length || length == path.size()) return {};
				std::ifstream file(std::filesystem::path(path.data()), std::ios::binary);
				if (!file) return {};

				BCRYPT_ALG_HANDLE algorithm{};
				if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
				const std::unique_ptr<void, decltype([](void* p) { BCryptCloseAlgorithmProvider(p, 0); })>
					ownedAlgorithm(algorithm);
				DWORD objectSize{}, received{};
				if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
					sizeof objectSize, &received, 0) < 0 || received != sizeof objectSize || !objectSize) return {};
				// The backing object must outlive the hash handle.
				std::vector<UCHAR> object(objectSize);
				BCRYPT_HASH_HANDLE hash{};
				if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0) return {};
				const std::unique_ptr<void, decltype(&BCryptDestroyHash)> ownedHash(hash, BCryptDestroyHash);
				std::array<UCHAR, 65536> buffer;
				while (file.read(reinterpret_cast<char*>(buffer.data()), buffer.size()) || file.gcount()) {
					if (BCryptHashData(hash, buffer.data(), static_cast<ULONG>(file.gcount()), 0) < 0) return {};
				}
				if (!file.eof() || file.bad()) return {};
				std::array<UCHAR, 32> digest;
				if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return {};

				constexpr char digits[] = "0123456789abcdef";
				std::string hex(digest.size() * 2, '0');
				for (size_t i = 0; i < digest.size(); ++i) {
					hex[2 * i] = digits[digest[i] >> 4];
					hex[2 * i + 1] = digits[digest[i] & 0xf];
				}
				return hex;
			}
		}

		void Create() {
			EXP_GetLogPath = (EXP_GetLogPath_t)GetProcAddress(CivVIModHandle, "EXP_GetLogPath");
		}

		bool SelectExecutableProfile() {
			const auto executableSha = ImageSha256(CivVIModHandle);
			const auto gameCoreSha = ImageSha256(GameCore);
			for (const auto& profile : profiles) {
				if (executableSha != profile.executableSha || gameCoreSha != profile.gameCoreSha) continue;
				const auto* entry = GetGlobalAt<const unsigned char*>(profile.publishEvents);
				if (std::memcmp(entry, profile.publishPrefix, sizeof profile.publishPrefix) != 0) return false;
				selectedProfile = &profile;
				MainLogger->Info("Qualified CE executable profile: {}", profile.name);
				return true;
			}
			return false;
		}

		const ExecutableProfile* GetExecutableProfile() { return selectedProfile; }
	}

	// Should only be called once
	void Create() {
		// Prologue; gather all program data
		GameProcessId = GetCurrentProcessId();
		GameProcess = GetCurrentProcess();
		CivVIModHandle = GetModuleHandle(NULL);

		// Create base runtime references
		Base::Create();

		// Get the log directory
		std::wstring buffer(MAX_PATH, L'\0');
		size_t charsWritten = Base::EXP_GetLogPath(buffer.data(), L"CE/");
		buffer.resize(charsWritten);
		std::filesystem::path logPath(buffer);

		// Create a CE/ subdirectory if it doesn't exist
		std::error_code ec;
		std::filesystem::create_directories(logPath, ec);

		// Initialize the logger library
		Log::Init(logPath);

		// Default main logger. Specialized Loggers can be created elsewhere.
		MainLogger = new Log::Logger("CE", "Community_Extension.log");

		// Load GameCore
		GameCore = LoadLibrary(TEXT("../../../DLC/Expansion2/Binaries/Win64/GameCore_XP2_FinalRelease.dll"));
		if (!GameCore) {
			Runtime::MainLogger->Error("DLC/Expansion2/Binaries/Win64/GameCore_XP2_FinalRelease.dll failed to load!");
			return;
		}
		GameCoreAddress = reinterpret_cast<uintptr_t>(GameCore);
	}

	void InitMinHook() {
		if (MH_Initialize() != MH_OK) {
			Runtime::MainLogger->Info("MH failed to init");
		}
		Runtime::MainLogger->Info("MH initialized successfully.");
	}

	bool InstallHookGroup(std::span<const HookSpec> hooks) {
		size_t created = 0;
		auto rollback = [&] {
			for (const auto& hook : hooks.first(created)) {
				// RemoveHook disables a hook if ApplyQueued enabled it before
				// reporting failure. Keep trampolines valid on cleanup failure.
				if (auto status = MH_RemoveHook(hook.target); status != MH_OK) {
					MainLogger->Error("Failed to remove {}: {}", hook.name, MH_StatusToString(status));
				}
				else {
					*hook.original = nullptr;
				}
			}
		};

		for (const auto& hook : hooks) {
			if (auto status = MH_CreateHook(hook.target, hook.detour, hook.original); status != MH_OK) {
				rollback();
				MainLogger->Error("Failed to create {}: {}", hook.name, MH_StatusToString(status));
				return false;
			}
			++created;
		}
		for (const auto& hook : hooks) {
			if (auto status = MH_QueueEnableHook(hook.target); status != MH_OK) {
				rollback();
				MainLogger->Error("Failed to queue {}: {}", hook.name, MH_StatusToString(status));
				return false;
			}
		}
		if (auto status = MH_ApplyQueued(); status != MH_OK) {
			rollback();
			MainLogger->Error("Failed to enable hook group: {}", MH_StatusToString(status));
			return false;
		}
		return true;
	}


namespace {
	struct InstructionCall {
		unsigned char* site{};
		void* relay{};
		DWORD originalProtection{};
		std::array<unsigned char, 5> original{};
		std::array<unsigned char, 5> replacement{};
	};

	// The adapter is installed once before the first game context is created.
	// Its target module is pinned. Context changes retire policy, not code.
	InstructionCall instructionCall;

	void* AllocateCallRelay(uintptr_t returnAddress, void (*target)()) {
		using AllocateNear = void* (WINAPI*)(HANDLE, void*, SIZE_T, ULONG, ULONG,
			MEM_EXTENDED_PARAMETER*, ULONG);
		const auto allocate = reinterpret_cast<AllocateNear>(
			GetProcAddress(GetModuleHandleW(L"KernelBase.dll"), "VirtualAlloc2"));
		if (!allocate)
			return nullptr;

		SYSTEM_INFO info{};
		GetSystemInfo(&info);
		const auto alignment = static_cast<uintptr_t>(info.dwAllocationGranularity);
		const auto center = returnAddress & ~(alignment - 1);
		// Stay inside the signed CALL range, allowing for the page and alignment.
		constexpr uintptr_t reach = 0x7fff0000;
		MEM_ADDRESS_REQUIREMENTS range{};
		range.LowestStartingAddress = reinterpret_cast<void*>((std::max)(
			center > reach ? center - reach : 0, reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress)));
		range.HighestEndingAddress = reinterpret_cast<void*>((std::min)(
			center + reach - 1, reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress)));
		MEM_EXTENDED_PARAMETER parameter{};
		parameter.Type = MemExtendedParameterAddressRequirements;
		parameter.Pointer = &range;
		auto* relay = static_cast<unsigned char*>(allocate(GetCurrentProcess(), nullptr,
			info.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &parameter, 1));
		if (!relay)
			return nullptr;

		// JMP [RIP+0] leaves every register and the caller's return address intact.
		constexpr unsigned char jump[] = {0xff, 0x25, 0, 0, 0, 0};
		std::memcpy(relay, jump, sizeof jump);
		std::memcpy(relay + sizeof jump, &target, sizeof target);
		DWORD previous{};
		if (!VirtualProtect(relay, info.dwPageSize, PAGE_EXECUTE_READ, &previous) ||
			!FlushInstructionCache(GetCurrentProcess(), relay, sizeof jump + sizeof target)) {
			VirtualFree(relay, 0, MEM_RELEASE);
			return nullptr;
		}
		return relay;
	}

	void RestoreInstructionCall() {
		auto& patch = instructionCall;
		if (!patch.relay)
			return;
		DWORD discarded{};
		if (std::memcmp(patch.site, patch.replacement.data(), patch.replacement.size()) == 0 &&
			VirtualProtect(patch.site, patch.original.size(), PAGE_EXECUTE_READWRITE, &discarded))
			std::memcpy(patch.site, patch.original.data(), patch.original.size());
		if (std::memcmp(patch.site, patch.original.data(), patch.original.size()) == 0) {
			const bool restored = VirtualProtect(patch.site, patch.original.size(),
				patch.originalProtection, &discarded);
			const bool flushed = FlushInstructionCache(GetCurrentProcess(), patch.site, patch.original.size());
			if (restored && flushed) {
				VirtualFree(patch.relay, 0, MEM_RELEASE);
				patch = {};
				return;
			}
		}
		MainLogger->Error("Could not restore the verified GameCore instruction call");
	}
}

	bool InstallInstructionCall(uintptr_t rva, std::span<const unsigned char, 5> expected,
		void (*target)()) {
		auto* site = GetGameCoreGlobalAt<unsigned char*>(rva);
		if (instructionCall.relay || !target || std::memcmp(site, expected.data(), expected.size()))
			return false;
		const auto returnAddress = reinterpret_cast<uintptr_t>(site + expected.size());
		void* relay = AllocateCallRelay(returnAddress, target);
		if (!relay)
			return false;
		const auto displacement = reinterpret_cast<intptr_t>(relay) - static_cast<intptr_t>(returnAddress);
		DWORD previous{};
		HMODULE targetModule{};
		// A failed rollback must never leave a CALL to an unloaded module.
		// Once installed, the adapter and its target are process-owned.
		constexpr DWORD pinTarget = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
		if (displacement < INT32_MIN || displacement > INT32_MAX ||
			!GetModuleHandleExW(pinTarget, reinterpret_cast<LPCWSTR>(target), &targetModule) ||
			!VirtualProtect(site, expected.size(), PAGE_EXECUTE_READWRITE, &previous)) {
			VirtualFree(relay, 0, MEM_RELEASE);
			return false;
		}

		auto& patch = instructionCall;
		patch.site = site;
		patch.relay = relay;
		patch.originalProtection = previous;
		std::copy(expected.begin(), expected.end(), patch.original.begin());
		patch.replacement[0] = 0xe8; // CALL rel32
		const auto relative = static_cast<int32_t>(displacement);
		std::memcpy(patch.replacement.data() + 1, &relative, sizeof relative);
		std::memcpy(site, patch.replacement.data(), patch.replacement.size());
		DWORD discarded{};
		if (FlushInstructionCache(GetCurrentProcess(), site, patch.replacement.size()) &&
			VirtualProtect(site, patch.replacement.size(), previous, &discarded))
			return true;

		// Retain the relay and ownership record unless both code and page
		// protection are restored. An unpublished target passes the score through.
		RestoreInstructionCall();
		return false;
	}

	// Call on dll exit
	void Destroy() {
		if (GameCore) {
			RestoreInstructionCall();
			MH_DisableHook(MH_ALL_HOOKS);
			MH_Uninitialize();
			FreeLibrary(GameCore);
			GameCore = NULL;
			GameCoreAddress = 0x0;
		}
	}

	void InitConsole() {
		if (!AllocConsole()) {
			MessageBoxW(NULL, L"Failed to create the console!", L"Error", MB_ICONERROR);
			return;
		}

		// Enable ANSI color escape sequence processing
		HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
		DWORD dwMode = 0;
		if (GetConsoleMode(hOut, &dwMode)) {
			SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
		}

		// Redirect standard input/output streams to the console
		FILE* fDummy;
		freopen_s(&fDummy, "CONIN$", "r", stdin);
		freopen_s(&fDummy, "CONOUT$", "w", stderr);
		freopen_s(&fDummy, "CONOUT$", "w", stdout);

		MainLogger->Info("Console initialized!");
	}

	void CloseConsole() {
		if (!FreeConsole()) {
			MessageBoxW(NULL, L"Failed to close the console!", L"Error", MB_ICONERROR);
		}
		else {
			std::cout << "Console closed successfully." << std::endl;
		}

		// Reset standard input/output streams
		freopen_s((FILE**)stdin, "NUL:", "r", stdin);
		freopen_s((FILE**)stdout, "NUL:", "w", stdout);
		freopen_s((FILE**)stderr, "NUL:", "w", stderr);

		// Clear the error state for each of the C++ standard streams after redirect.
		std::cin.clear();
		std::cout.clear();
		std::cerr.clear();
	}

	BOOL WriteCodeToProcess(uintptr_t address, byte* bytes, size_t sizeOfBytes) {
		DWORD oldProtect;
		LPVOID lpBaseAddress = reinterpret_cast<LPVOID>(address);

		if (!VirtualProtectEx(GameProcess, lpBaseAddress, sizeOfBytes, PAGE_EXECUTE_READWRITE, &oldProtect)) {
			MainLogger->Error("Failed to change memory protection: {}", GetLastError());
			return FALSE;
		}

		SIZE_T numberOfBytesWritten;
		BOOL result = WriteProcessMemory(GameProcess, lpBaseAddress, bytes, sizeOfBytes, &numberOfBytesWritten);

		if (!result || numberOfBytesWritten != sizeOfBytes) {
			MainLogger->Error("Failed to write to memory: {}", GetLastError());
			VirtualProtectEx(GameProcess, lpBaseAddress, sizeOfBytes, oldProtect, &oldProtect);
			return FALSE;
		}

		VirtualProtectEx(GameProcess, lpBaseAddress, sizeOfBytes, oldProtect, &oldProtect);

		return TRUE;
	}

	BOOL WriteCodeToGameCore(uintptr_t address, byte* bytes, size_t sizeOfBytes) {
		return WriteCodeToProcess(address + GameCoreAddress, bytes, sizeOfBytes);
	}
};
