#include "ForgeUI.h"
#include "Runtime.h"

namespace ForgeUI {
	Types::GetLock GetLock;
	Types::ReleaseLock ReleaseLock;
	ForgeUI_UIManager* manager;

	void Create() {
		using namespace Runtime;

		const auto& profile = *Base::GetExecutableProfile();
		GetLock = GetGlobalAt<Types::GetLock>(profile.forgeGetLock);
		ReleaseLock = GetGlobalAt<Types::ReleaseLock>(profile.forgeReleaseLock);
		manager = GetGlobalAt<ForgeUI_UIManager*>(profile.forgeManagerSlot);
	}
}
