#pragma once
#include "HavokScript.h"

namespace ForgeUI {
	class ForgeUI_UIManager;
	namespace Types {
		typedef void (*GetLock)(ForgeUI_UIManager*);
		typedef void (*ReleaseLock)(ForgeUI_UIManager*);
	}
	
	extern Types::GetLock GetLock;

	extern Types::ReleaseLock ReleaseLock;

	extern ForgeUI_UIManager* manager;

	extern void Create();
}
