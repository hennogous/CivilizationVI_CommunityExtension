#pragma once
#include "MinHook.h"
#include "ProxyTypes.h"
#include <iostream>
#include "capstone/capstone.h"
#include "Log.h"
#include <span>

namespace Runtime {
	extern HMODULE GameCore;
	extern uintptr_t GameCoreAddress;
	extern DWORD GameProcessId;
	extern HANDLE GameProcess;
	extern HMODULE CivVIModHandle;

	extern Log::Logger* MainLogger;

	namespace Base {
		struct ExecutableProfile {
			const char* name;
			const char* executableSha;
			const char* gameCoreSha;
			uintptr_t publishEvents;
			uintptr_t luaCriticalSection;
			uintptr_t forgeGetLock;
			uintptr_t forgeReleaseLock;
			uintptr_t forgeManagerSlot;
			unsigned char publishPrefix[24];
		};

		using EXP_GetLogPath_t = size_t(__fastcall*)(wchar_t* pszOutBuffer, const wchar_t* pszSubPath);
		extern EXP_GetLogPath_t EXP_GetLogPath;

		extern void Create();
		extern bool SelectExecutableProfile();
		extern const ExecutableProfile* GetExecutableProfile();
	}

	// Should only be called once
	extern void Create();

	// Call on dll exit
	extern void Destroy();

	extern void InitMinHook();

	template <typename T>
	T GetGameCoreGlobalAt(uintptr_t address) {
		return reinterpret_cast<T>(GameCoreAddress + address);
	}

	template <typename T>
	T GetGlobalAt(uintptr_t address) {
		return reinterpret_cast<T>((uintptr_t)CivVIModHandle + address);
	}

	template <typename T>
	void CreateHook(LPVOID pTarget, LPVOID pDetour, T** ppOriginal) {
		MH_STATUS status;
		if ((status = MH_CreateHook(pTarget, pDetour, reinterpret_cast<LPVOID*>(ppOriginal))) != MH_OK) {
			Runtime::MainLogger->Error("Failed to load hook! {}", MH_StatusToString(status));
		}
		if ((status = MH_EnableHook(pTarget)) != MH_OK) {
			Runtime::MainLogger->Info("Failed to enable hook! {}", MH_StatusToString(status));
		}
	}

	// Use for related hooks that must have valid trampolines before any detour
	// can execute. Install during serialized Runtime initialization, with no
	// other queued MinHook operations. Failure rolls back this group's hooks.
	struct HookSpec {
		const char* name;
		LPVOID target;
		LPVOID detour;
		LPVOID* original;
	};

	template <typename Function>
	HookSpec DescribeHook(const char* name, Function* target, Function* detour, Function** original) {
		return {name, reinterpret_cast<LPVOID>(target), reinterpret_cast<LPVOID>(detour),
			reinterpret_cast<LPVOID*>(original)};
	}

	bool InstallHookGroup(std::span<const HookSpec> hooks);

	// Replace one verified five-byte instruction with a near CALL. The target
	// must replay the instruction and obey the site's ABI. Install during
	// serialized initialization. Runtime pins the adapter's target module and
	// owns the sole relay for the process. A game-context reset retires policy.
	bool InstallInstructionCall(uintptr_t rva, std::span<const unsigned char, 5> expected,
		void (*target)());

	extern void InitConsole();
	extern void CloseConsole();

	extern BOOL WriteCodeToGameCore(uintptr_t address, byte* bytes, size_t sizeOfBytes);
	extern BOOL WriteCodeToProcess(uintptr_t address, byte* bytes, size_t sizeOfBytes);
};