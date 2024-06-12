#include "threading.h"
#include "shared-table.h"
#include "garbage-collector.h"
#include "channel.h"
#include "thread-runner.h"

#include <lua.hpp>

namespace
{

	effil::SharedTable globalTable = effil::GC::instance().create<effil::SharedTable>();

	std::string getLuaTypename(const sol::stack_object& obj)
	{
		return effil::luaTypename(obj);
	}

	size_t luaSize(const sol::stack_object& obj)
	{
		if (obj.is<effil::SharedTable>())
			return effil::SharedTable::luaSize(obj);
		else if (obj.is<effil::Channel>())
			return obj.as<effil::Channel>().size();

		throw effil::Exception() << "Unsupported type "
								 << effil::luaTypename(obj) << " for effil.size()";
	}

	sol::object luaDump(sol::this_state lua, const sol::stack_object& obj)
	{
		if (obj.is<effil::SharedTable>())
		{
			effil::BaseHolder::DumpCache cache;
			return obj.as<effil::SharedTable>().luaDump(lua, cache);
		}
		else if (obj.get_type() == sol::type::table)
		{
			return obj;
		}

		throw effil::Exception() << "bad argument #1 to 'effil.dump' (table expected, got "
								 << effil::luaTypename(obj) << ")";
	}

} // namespace

extern "C"
#ifdef _WIN32
	__declspec(dllexport)
#endif
		int luaopen_effil(lua_State* L)
{
	sol::state_view lua(L);

	static char uniqueRegistryKeyValue = 'a';
	char*		uniqueRegistryKey = &uniqueRegistryKeyValue;
	const auto	registryKey = sol::make_light(uniqueRegistryKey);

	if (lua.registry()[registryKey] == 1)
	{
		sol::stack::push(lua, effil::EffilApiMarker());
		return 1;
	}

	effil::Thread::exportAPI(lua);

	// const auto topBefore = lua_gettop(L);

	auto type = lua.new_usertype<effil::EffilApiMarker>(sol::no_constructor);

	type["version"] = sol::make_object(lua, "1.0.0");
	type["G"] = sol::make_object(lua, globalTable);

	type["channel"] = effil::Channel::exportAPI(lua);
	type["table"] = effil::SharedTable::exportAPI(lua);
	type["thread"] = effil::ThreadRunner::exportAPI(lua);
	type["gc"] = effil::GC::exportAPI(lua);

	type["thread_id"] = effil::this_thread::threadId;
	type["sleep"] = effil::this_thread::sleep;
	type["yield"] = effil::this_thread::yield;
	type["rawset"] = effil::SharedTable::luaRawSet;
	type["rawget"] = effil::SharedTable::luaRawGet;
	type["setmetatable"] = effil::SharedTable::luaSetMetatable;
	type["getmetatable"] = effil::SharedTable::luaGetMetatable;
	type["type"] = getLuaTypename;
	type["pairs"] = effil::SharedTable::globalLuaPairs;
	type["ipairs"] = effil::SharedTable::globalLuaIPairs;
	type["next"] = effil::SharedTable::globalLuaNext;
	type["size"] = luaSize;
	type["dump"] = luaDump;
	type["hardware_threads"] = std::thread::hardware_concurrency;

	// if (topBefore != lua_gettop(L))
	//     sol::stack::pop_n(L, lua_gettop(L) - topBefore);

	lua.registry()[registryKey] = 1;

	sol::stack::push(lua, effil::EffilApiMarker());
	return 1;
}
