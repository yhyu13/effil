#include "shared-table.h"
#include "function.h"

#include "utils.h"

#include <cassert>
#include <shared_mutex>

namespace effil
{

	namespace
	{

		typedef std::unique_lock<SpinMutex> UniqueLock;
		typedef std::shared_lock<SpinMutex> SharedLock;

		template <typename SolObject>
		bool isSharedTable(const SolObject& obj)
		{
			return obj.valid() && obj.get_type() == sol::type::userdata && obj.template is<SharedTable>();
		}

		template <typename SolObject>
		bool isAnyTable(const SolObject& obj)
		{
			return obj.valid() && ((obj.get_type() == sol::type::userdata && obj.template is<SharedTable>()) || obj.get_type() == sol::type::table);
		}

	} // namespace

	struct Sample
	{
		sol::object index(const sol::stack_object& key)
		{
			std::cout << "index called with key '" << key.as<std::string>() << std::endl;
			return sol::object();
		}
		void newIndex(const sol::stack_object& key, const sol::stack_object&)
		{
			std::cout << "newIndex called with key '" << key.as<std::string>() << std::endl;
		}
	};
	sol::usertype<SharedTable> SharedTable::exportAPI(sol::state_view& lua)
	{
		auto sampleType = lua.new_usertype<Sample>("sample");

		sampleType[sol::meta_function::new_index] = &Sample::newIndex;
		// sampleType[sol::meta_function::index] = &Sample::index;

		auto type = lua.new_usertype<SharedTable>(sol::no_constructor);

		type[sol::call_constructor] = sol::factories(
			[](sol::this_state lua, const sol::stack_object& tbl) {
				REQUIRE(tbl.get_type() == sol::type::table)
					<< "Unexpected type for effil.table, table expected got: "
					<< lua_typename(lua, (int)tbl.get_type());
				return createStoredObject(tbl)->unpack(lua);
			},
			[](sol::this_state lua) {
				return sol::make_object(lua, GC::instance().create<SharedTable>());
			});
		type[sol::meta_function::pairs] = &SharedTable::luaPairs;
		type[sol::meta_function::ipairs] = &SharedTable::luaIPairs;
		type[sol::meta_function::new_index] = &SharedTable::luaNewIndex;
		type[sol::meta_function::index] = &SharedTable::luaIndex;
		type[sol::meta_function::static_new_index] = &SharedTable::luaNewIndex;
		type[sol::meta_function::static_index] = &SharedTable::luaIndex;
		type[sol::meta_function::length] = &SharedTable::luaLength;
		type[sol::meta_function::to_string] = &SharedTable::luaToString;
		type[sol::meta_function::addition] = &SharedTable::luaAdd;
		type[sol::meta_function::subtraction] = &SharedTable::luaSub;
		type[sol::meta_function::multiplication] = &SharedTable::luaMul;
		type[sol::meta_function::division] = &SharedTable::luaDiv;
		type[sol::meta_function::modulus] = &SharedTable::luaMod;
		type[sol::meta_function::power_of] = &SharedTable::luaPow;
		type[sol::meta_function::concatenation] = &SharedTable::luaConcat;
		type[sol::meta_function::less_than] = &SharedTable::luaLt;
		type[sol::meta_function::unary_minus] = &SharedTable::luaUnm;
		type[sol::meta_function::call] = &SharedTable::luaCall;
		type[sol::meta_function::equal_to] = &SharedTable::luaEq;
		type[sol::meta_function::less_than_or_equal_to] = &SharedTable::luaLe;
		return type;
	}

	void SharedTable::set(StoredObject&& key, StoredObject&& value)
	{
		UniqueLock g(ctx_->lock);

		ctx_->addReference(key->gcHandle());
		ctx_->addReference(value->gcHandle());

		key->releaseStrongReference();
		value->releaseStrongReference();

		ctx_->entries[std::move(key)] = std::move(value);
	}

	sol::object SharedTable::get(const StoredObject& key, sol::this_state state) const
	{
		SharedLock g(ctx_->lock);
		const auto val = ctx_->entries.find(key);
		if (val == ctx_->entries.end())
		{
			return sol::nil;
		}
		else
		{
			return val->second->unpack(state);
		}
	}

	void SharedTable::rawSet(const sol::stack_object& luaKey, const sol::stack_object& luaValue)
	{
		REQUIRE(luaKey.valid()) << "Indexing by nil";

		StoredObject key = createStoredObject(luaKey);
		if (luaValue.get_type() == sol::type::nil)
		{
			UniqueLock g(ctx_->lock);

			// in this case object is not obligatory to own data
			auto it = ctx_->entries.find(key);
			if (it != ctx_->entries.end())
			{
				ctx_->removeReference(it->first->gcHandle());
				ctx_->removeReference(it->second->gcHandle());
				ctx_->entries.erase(it);
			}
		}
		else
		{
			set(std::move(key), createStoredObject(luaValue));
		}
	}

	sol::object SharedTable::rawGet(const sol::stack_object& luaKey, sol::this_state state) const
	{
		REQUIRE(luaKey.valid()) << "Indexing by nil";
		StoredObject key = createStoredObject(luaKey);
		return get(key, state);
	}

	sol::object SharedTable::luaDump(sol::this_state state, BaseHolder::DumpCache& cache) const
	{
		const auto iter = cache.find(handle());
		if (iter == cache.end())
		{
			SharedLock lock(ctx_->lock);

			auto result = sol::table::create(state.L);
			cache.insert(iter, { handle(), result.registry_index() });
			for (const auto& pair : ctx_->entries)
			{
				result.set(pair.first->convertToLua(state, cache),
					pair.second->convertToLua(state, cache));
			}
			if (ctx_->metatable)
			{
				const auto mt = GC::instance().get<SharedTable>(ctx_->metatable);
				lock.unlock();

				result[sol::metatable_key] = mt.luaDump(state, cache);
			}
			return result;
		}
		return sol::table(state.L, sol::ref_index(iter->second));
	}

/*
 * Lua Meta API methods
 */
#define DEFFINE_METAMETHOD_CALL_0(methodName) DEFFINE_METAMETHOD_CALL(methodName, *this)
#define DEFFINE_METAMETHOD_CALL(methodName, ...)                                          \
	{                                                                                     \
		SharedLock lock(ctx_->lock);                                                      \
		if (ctx_->metatable != GCNull)                                                    \
		{                                                                                 \
			auto tableHolder = GC::instance().get<SharedTable>(ctx_->metatable);          \
			lock.unlock();                                                                \
			sol::object handler = tableHolder.get(createStoredObject(methodName), state); \
			if (handler.valid() && handler.get_type() == sol::type::function)             \
			{                                                                             \
				sol::function handlerFunc = handler;                                      \
				return handlerFunc(__VA_ARGS__);                                          \
			}                                                                             \
		}                                                                                 \
	}

#define PROXY_METAMETHOD_IMPL(tableMethod, methodName, errMsg)                            \
	sol::object SharedTable::tableMethod(sol::this_state state,                           \
		const sol::stack_object& leftObject, const sol::stack_object& rightObject)        \
	{                                                                                     \
		return basicBinaryMetaMethod(methodName, errMsg, state, leftObject, rightObject); \
	}

	namespace
	{
		const std::string ARITHMETIC_ERR_MSG = "attempt to perform arithmetic on a effil::table value";
		const std::string COMPARE_ERR_MSG = "attempt to compare a effil::table value";
		const std::string CONCAT_ERR_MSG = "attempt to concatenate a effil::table value";
	} // namespace

	sol::object SharedTable::basicBinaryMetaMethod(const std::string& metamethodName, const std::string& errMsg,
		sol::this_state state, const sol::stack_object& leftObject, const sol::stack_object& rightObject)
	{
		if (isSharedTable(leftObject))
		{
			SharedTable table = leftObject.as<SharedTable>();
			auto		ctx_ = table.ctx_;
			DEFFINE_METAMETHOD_CALL(metamethodName, table, rightObject)
		}
		if (isSharedTable(rightObject))
		{
			SharedTable table = rightObject.as<SharedTable>();
			auto		ctx_ = table.ctx_;
			DEFFINE_METAMETHOD_CALL(metamethodName, leftObject, table)
		}
		throw Exception() << errMsg;
	}

	PROXY_METAMETHOD_IMPL(luaConcat, "__concat", CONCAT_ERR_MSG)
	PROXY_METAMETHOD_IMPL(luaAdd, "__add", ARITHMETIC_ERR_MSG)
	PROXY_METAMETHOD_IMPL(luaSub, "__sub", ARITHMETIC_ERR_MSG)
	PROXY_METAMETHOD_IMPL(luaMul, "__mul", ARITHMETIC_ERR_MSG)
	PROXY_METAMETHOD_IMPL(luaDiv, "__div", ARITHMETIC_ERR_MSG)
	PROXY_METAMETHOD_IMPL(luaMod, "__mod", ARITHMETIC_ERR_MSG)
	PROXY_METAMETHOD_IMPL(luaPow, "__pow", ARITHMETIC_ERR_MSG)
	PROXY_METAMETHOD_IMPL(luaLe, "__le", ARITHMETIC_ERR_MSG)
	PROXY_METAMETHOD_IMPL(luaLt, "__lt", ARITHMETIC_ERR_MSG)

	sol::object SharedTable::luaEq(sol::this_state state, const sol::stack_object& leftObject,
		const sol::stack_object& rightObject)
	{
		if (isSharedTable(leftObject) && isSharedTable(rightObject))
		{
			{
				SharedTable table = leftObject.as<SharedTable>();
				auto		ctx_ = table.ctx_;
				DEFFINE_METAMETHOD_CALL("__eq", table, rightObject)
			}
			{
				SharedTable table = rightObject.as<SharedTable>();
				auto		ctx_ = table.ctx_;
				DEFFINE_METAMETHOD_CALL("__eq", leftObject, table)
			}
			const bool isEqual = leftObject.as<SharedTable>().handle() == rightObject.as<SharedTable>().handle();
			return sol::make_object(state, isEqual);
		}
		return sol::make_object(state, false);
	}

	sol::object SharedTable::luaUnm(sol::this_state state)
	{
		DEFFINE_METAMETHOD_CALL_0("__unm")
		throw Exception() << ARITHMETIC_ERR_MSG;
	}

	void SharedTable::luaNewIndex(const sol::stack_object& luaKey, const sol::stack_object& luaValue, sol::this_state state)
	{
		std::cout << "EE: " << (int)luaKey.get_type() << ", " << (int)luaValue.get_type() << std::endl;
		{
			SharedLock lock(ctx_->lock);
			if (ctx_->metatable != GCNull)
			{
				auto tableHolder = GC::instance().get<SharedTable>(ctx_->metatable);
				lock.unlock();
				sol::object handler = tableHolder.get(createStoredObject("__newindex"), state);
				if (handler.valid() && handler.get_type() == sol::type::function)
				{
					sol::function handlerFunc = handler;
					handlerFunc(*this, luaKey, luaValue);
					return;
				}
			}
		}
		try
		{
			std::cout << (int)luaKey.get_type() << ", " << (int)luaValue.get_type() << std::endl;
			rawSet(luaKey, luaValue);
		}
		RETHROW_WITH_PREFIX("effil.table");
	}

	sol::object SharedTable::luaIndex(const sol::stack_object& luaKey, sol::this_state state) const
	{
		REQUIRE(luaKey.valid()) << "Indexing by nil";
		try
		{
			StoredObject key = createStoredObject(luaKey);
			if (sol::object result = get(key, state))
			{
				return result;
			}
		}
		RETHROW_WITH_PREFIX("effil.table");

		SharedLock lock(ctx_->lock);
		if (ctx_->metatable != GCNull)
		{
			const auto tableHolder = GC::instance().get<SharedTable>(ctx_->metatable);
			lock.unlock();

			SharedLock mt_lock(tableHolder.ctx_->lock);
			const auto iter = tableHolder.ctx_->entries.find(createStoredObject("__index"));
			std::cout << "ASD=: " << bool(iter != tableHolder.ctx_->entries.end()) << std::endl;
			if (iter != tableHolder.ctx_->entries.end())
			{
				std::cout << "ASD==: " << storedObjectTo<Function>(iter->second).has_value() << std::endl;
				if (const auto tbl = storedObjectTo<SharedTable>(iter->second))
				{
					mt_lock.unlock();
					return tbl->luaIndex(luaKey, state);
				}
				else if (const auto func = storedObjectTo<Function>(iter->second))
				{
					mt_lock.unlock();
					return func->loadFunction(state).as<sol::function>()(*this, luaKey);
				}
			}
		}
		return sol::nil;
	}

	StoredArray SharedTable::luaCall(sol::this_state state, const sol::variadic_args& args)
	{
		SharedLock lock(ctx_->lock);
		if (ctx_->metatable != GCNull)
		{
			auto		  metatable = GC::instance().get<SharedTable>(ctx_->metatable);
			sol::function handler = metatable.get(createStoredObject(std::string("__call")), state);
			lock.unlock();
			if (handler.valid())
			{
				StoredArray			 storedResults;
				const int			 savedStackTop = lua_gettop(state);
				sol::function_result callResults = handler(*this, args);
				(void)callResults;
				sol::variadic_args funcReturns(state, savedStackTop - lua_gettop(state));
				for (const auto& param : funcReturns)
					storedResults.emplace_back(createStoredObject(param.get<sol::object>()));
				return storedResults;
			}
		}
		throw Exception() << "attempt to call a table";
	}

	sol::object SharedTable::luaToString(sol::this_state state)
	{
		DEFFINE_METAMETHOD_CALL_0("__tostring");
		std::stringstream ss;
		ss << "effil.table: " << ctx_.get();
		return sol::make_object(state, ss.str());
	}

	sol::object SharedTable::luaLength(sol::this_state state)
	{
		DEFFINE_METAMETHOD_CALL_0("__len");
		SharedLock					  g(ctx_->lock);
		size_t						  len = 0u;
		sol::optional<LUA_INDEX_TYPE> value;
		auto						  iter = ctx_->entries.find(createStoredObject(static_cast<LUA_INDEX_TYPE>(1)));
		if (iter != ctx_->entries.end())
		{
			do
			{
				++len;
				++iter;
			}
			while ((iter != ctx_->entries.end()) && (value = storedObjectToIndexType(iter->first)) && (static_cast<size_t>(value.value()) == len + 1));
		}
		return sol::make_object(state, len);
	}

	SharedTable::PairsIterator SharedTable::getNext(const sol::object& key, sol::this_state lua) const
	{
		SharedLock g(ctx_->lock);
		if (key)
		{
			auto obj = createStoredObject(key);
			auto upper = ctx_->entries.upper_bound(obj);
			if (upper != ctx_->entries.end())
				return PairsIterator(upper->first->unpack(lua), upper->second->unpack(lua));
		}
		else
		{
			if (!ctx_->entries.empty())
			{
				const auto& begin = ctx_->entries.begin();
				return PairsIterator(begin->first->unpack(lua), begin->second->unpack(lua));
			}
		}
		return PairsIterator(sol::nil, sol::nil);
	}

	SharedTable::PairsIterator SharedTable::luaPairs(sol::this_state state)
	{
		DEFFINE_METAMETHOD_CALL_0("__pairs");
		auto next = [](sol::this_state state, SharedTable table, sol::stack_object key) { return table.getNext(key, state); };
		return PairsIterator(
			sol::make_object(state, std::function<PairsIterator(sol::this_state state, SharedTable table, sol::stack_object key)>(next)).as<sol::function>(),
			sol::make_object(state, *this));
	}

	static std::pair<sol::object, sol::object> ipairsNext(sol::this_state lua, SharedTable table, const sol::optional<LUA_INDEX_TYPE>& key)
	{
		size_t		index = key ? key.value() + 1 : 1;
		auto		objKey = createStoredObject(static_cast<LUA_INDEX_TYPE>(index));
		sol::object value = table.get(objKey, lua);
		if (!value.valid())
			return std::pair<sol::object, sol::object>(sol::nil, sol::nil);
		return std::pair<sol::object, sol::object>(objKey->unpack(lua), value);
	}

	SharedTable::PairsIterator SharedTable::luaIPairs(sol::this_state state)
	{
		DEFFINE_METAMETHOD_CALL_0("__ipairs");
		return PairsIterator(sol::make_object(state, ipairsNext).as<sol::function>(),
			sol::make_object(state, *this));
	}

	SharedTable SharedTable::setMetatable(const sol::optional<SharedTable>& metaTable)
	{
		UniqueLock lock(ctx_->lock);
		if (ctx_->metatable != GCNull)
		{
			ctx_->removeReference(ctx_->metatable);
			ctx_->metatable = GCNull;
		}

		if (metaTable)
		{
			ctx_->metatable = metaTable->handle();
			ctx_->addReference(ctx_->metatable);
		}
		return *this;
	}

	/*
	 * Lua static API functions
	 */

	SharedTable SharedTable::luaSetMetatable(const sol::stack_object& tbl, const sol::stack_object& mt)
	{
		REQUIRE(isAnyTable(tbl))
			<< "bad argument #1 to 'effil.setmetatable' (table expected, got "
			<< luaTypename(tbl) << ")";
		REQUIRE(isAnyTable(mt) || !mt.valid())
			<< "bad argument #2 to 'effil.setmetatable' (table or nil expected, got "
			<< luaTypename(mt) << ")";

		SolTableToShared		   cache;
		SharedTable				   table = GC::instance().get<SharedTable>(createStoredObject(tbl, cache)->gcHandle());
		sol::optional<SharedTable> metatable;
		if (mt.valid())
		{
			metatable = GC::instance().get<SharedTable>(createStoredObject(mt, cache)->gcHandle());
		}
		return table.setMetatable(metatable);
	}

	sol::object SharedTable::luaGetMetatable(const sol::stack_object& tbl, sol::this_state state)
	{
		REQUIRE(isSharedTable(tbl)) << "bad argument #1 to 'effil.getmetatable' (effil.table expected, got " << luaTypename(tbl) << ")";
		auto& stable = tbl.as<SharedTable>();

		SharedLock lock(stable.ctx_->lock);
		return stable.ctx_->metatable == GCNull ? sol::nil : sol::make_object(state, GC::instance().get<SharedTable>(stable.ctx_->metatable));
	}

	sol::object SharedTable::luaRawGet(const sol::stack_object& tbl, const sol::stack_object& key, sol::this_state state)
	{
		REQUIRE(isSharedTable(tbl)) << "bad argument #1 to 'effil.rawget' (effil.table expected, got " << luaTypename(tbl) << ")";
		try
		{
			return tbl.as<SharedTable>().rawGet(key, state);
		}
		RETHROW_WITH_PREFIX("effil.rawget");
	}

	SharedTable SharedTable::luaRawSet(const sol::stack_object& tbl, const sol::stack_object& key, const sol::stack_object& value)
	{
		REQUIRE(isSharedTable(tbl)) << "bad argument #1 to 'effil.rawset' (effil.table expected, got " << luaTypename(tbl) << ")";
		try
		{
			auto& stable = tbl.as<SharedTable>();
			stable.rawSet(key, value);
			return stable;
		}
		RETHROW_WITH_PREFIX("effil.rawset");
	}

	size_t SharedTable::luaSize(const sol::stack_object& tbl)
	{
		REQUIRE(isSharedTable(tbl)) << "bad argument #1 to 'effil.size' (effil.table expected, got " << luaTypename(tbl) << ")";
		try
		{
			auto&	   stable = tbl.as<SharedTable>();
			SharedLock g(stable.ctx_->lock);
			return stable.ctx_->entries.size();
		}
		RETHROW_WITH_PREFIX("effil.size");
	}

	SharedTable::PairsIterator SharedTable::globalLuaPairs(sol::this_state state, const sol::stack_object& obj)
	{
		REQUIRE(isSharedTable(obj)) << "bad argument #1 to 'effil.pairs' (effil.table expected, got " << luaTypename(obj) << ")";
		auto& tbl = obj.as<SharedTable>();
		return tbl.luaPairs(state);
	}

	SharedTable::PairsIterator SharedTable::globalLuaIPairs(sol::this_state state, const sol::stack_object& obj)
	{
		REQUIRE(isSharedTable(obj)) << "bad argument #1 to 'effil.ipairs' (effil.table expected, got " << luaTypename(obj) << ")";
		auto& tbl = obj.as<SharedTable>();
		return tbl.luaIPairs(state);
	}

	SharedTable::PairsIterator SharedTable::globalLuaNext(sol::this_state state, const sol::stack_object& obj, const sol::stack_object& key)
	{
		REQUIRE(isSharedTable(obj)) << "bad argument #1 to 'effil.next' (effil.table expected, got " << luaTypename(obj) << ")";
		const auto& tbl = obj.as<SharedTable>();
		return tbl.getNext(key, state);
	}

#undef DEFFINE_METAMETHOD_CALL_0
#undef DEFFINE_METAMETHOD_CALL
#undef PROXY_METAMETHOD_IMPL

} // namespace effil
