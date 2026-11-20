#pragma once
#include "HavokScript.h"
#include "Unit.h"

namespace UnitManager {
	typedef class Manager;
	namespace Types {
		typedef void(__thiscall* ChangeOwner)(Manager* manager, Unit::Instance* unit, int playerId, bool b1, bool b2, Unit::Instance** unit2Ref);
		typedef Manager* (__cdecl* Get)(void);
	}

	extern int lChangeOwner(hks::lua_State* L);
	extern int lGetInstance(hks::lua_State* L);

	constexpr uintptr_t CHANGE_OWNER_OFFSET = 0x64f450;
	extern Types::ChangeOwner ChangeOwner;

	constexpr uintptr_t GET_OFFSET = 0x650020;
	extern Types::Get Get;

	constexpr uintptr_t REGISTER_MEMBERS_OFFSET = 0x7009c0;
	extern void RegisterMembers(hks::lua_State* L);

	extern void Create();
}