#include "scripting/lua_sandbox.hpp"

extern "C"
{
#include <lauxlib.h>
#include <lobject.h>
#include <ltable.h>
#include <lualib.h>
}

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>

namespace kcd2o::scripting
{
	namespace
	{
		char sandbox_registry_key;
		char loaded_modules_registry_key;

		void ensure_lua_fork_sentinels() noexcept
		{
			// KCD2's Lua fork redirects these two internal sentinels so the
			// injected client can point them at the game's own Lua objects.
			// A standalone server has no game loader to initialize them.
			static const Node local_dummy_node = {
			    {{nullptr}, LUA_TNIL},
			    {{{nullptr}, LUA_TNIL, nullptr}}};
			if (luaO_nilobject_external_address == 0)
				luaO_nilobject_external_address =
				    reinterpret_cast<std::intptr_t>(&luaO_nilobject_);
			if (dummynode_external_address == 0)
				dummynode_external_address =
				    reinterpret_cast<std::intptr_t>(&local_dummy_node);
		}

		int absolute_index(lua_State *state, int index)
		{
			return index > 0 || index <= LUA_REGISTRYINDEX
			    ? index
			    : lua_gettop(state) + index + 1;
		}

		std::string normalize_module(std::string_view name)
		{
			if (name.empty() || name.size() > 200 || name.front() == '.'
			    || name.front() == '/' || name.front() == '\\'
			    || name.contains("..") || name.contains(':')
			    || name.contains('\\'))
				return {};
			std::string path(name);
			std::ranges::replace(path, '.', '/');
			path += ".lua";
			return path;
		}

		nlohmann::json to_json_impl(
		    lua_State *state,
		    int index,
		    std::size_t depth,
		    std::size_t &items)
		{
			if (depth > 16 || ++items > 2048)
				throw std::runtime_error("Lua value exceeds JSON complexity limit");
			index = absolute_index(state, index);
			switch (lua_type(state, index))
			{
			case LUA_TNIL:
				return nullptr;
			case LUA_TBOOLEAN:
				return lua_toboolean(state, index) != 0;
			case LUA_TNUMBER:
				return lua_tonumber(state, index);
			case LUA_TSTRING:
			{
				std::size_t length{};
				const auto *value = lua_tolstring(state, index, &length);
				if (length > 32 * 1024)
					throw std::runtime_error("Lua string exceeds JSON limit");
				return std::string(value, length);
			}
			case LUA_TTABLE:
			{
				const auto length = static_cast<std::size_t>(lua_objlen(state, index));
				bool array = length > 0;
				std::size_t entries{};
				lua_pushnil(state);
				while (lua_next(state, index) != 0)
				{
					++entries;
					if (lua_type(state, -2) != LUA_TNUMBER)
						array = false;
					else
					{
						const auto number = lua_tonumber(state, -2);
						if (number < 1 || number > static_cast<lua_Number>(length)
						    || number != std::floor(number))
							array = false;
					}
					lua_pop(state, 1);
				}
				if (entries == 0)
					return nlohmann::json::object();
				if (array && entries == length)
				{
					nlohmann::json result = nlohmann::json::array();
					for (std::size_t item{}; item < length; ++item)
					{
						lua_rawgeti(state, index, static_cast<int>(item + 1));
						result.push_back(to_json_impl(state, -1, depth + 1, items));
						lua_pop(state, 1);
					}
					return result;
				}
				nlohmann::json result = nlohmann::json::object();
				lua_pushnil(state);
				while (lua_next(state, index) != 0)
				{
					if (lua_type(state, -2) != LUA_TSTRING)
					{
						lua_pop(state, 2);
						throw std::runtime_error("Lua JSON object keys must be strings");
					}
					std::size_t length{};
					const auto *key = lua_tolstring(state, -2, &length);
					if (length == 0 || length > 128)
					{
						lua_pop(state, 2);
						throw std::runtime_error("Lua JSON object key is invalid");
					}
					result[std::string(key, length)] =
					    to_json_impl(state, -1, depth + 1, items);
					lua_pop(state, 1);
				}
				return result;
			}
			default:
				throw std::runtime_error("Lua value cannot be represented as JSON");
			}
		}

		void push_json_impl(lua_State *state, const nlohmann::json &value)
		{
			if (value.is_null())
				lua_pushnil(state);
			else if (value.is_boolean())
				lua_pushboolean(state, value.get<bool>());
			else if (value.is_number())
				lua_pushnumber(state, value.get<lua_Number>());
			else if (value.is_string())
			{
				const auto &text = value.get_ref<const std::string &>();
				lua_pushlstring(state, text.data(), text.size());
			}
			else if (value.is_array())
			{
				lua_createtable(state, static_cast<int>(value.size()), 0);
				for (std::size_t index{}; index < value.size(); ++index)
				{
					push_json_impl(state, value[index]);
					lua_rawseti(state, -2, static_cast<int>(index + 1));
				}
			}
			else
			{
				lua_createtable(state, 0, static_cast<int>(value.size()));
				for (const auto &[key, item] : value.items())
				{
					push_json_impl(state, item);
					lua_setfield(state, -2, key.c_str());
				}
			}
		}

		void validate_json_impl(
		    const nlohmann::json &value,
		    std::size_t depth,
		    std::size_t &items)
		{
			if (depth > 16 || ++items > 2048)
				throw std::runtime_error("JSON value exceeds Lua complexity limit");
			if (value.is_string()
			    && value.get_ref<const std::string &>().size() > 32 * 1024)
				throw std::runtime_error("JSON string exceeds Lua limit");
			if (value.is_array())
				for (const auto &item : value)
					validate_json_impl(item, depth + 1, items);
			else if (value.is_object())
				for (const auto &[key, item] : value.items())
				{
					if (key.empty() || key.size() > 128)
						throw std::runtime_error("JSON object key exceeds Lua limit");
					validate_json_impl(item, depth + 1, items);
				}
		}
	}

	lua_sandbox::lua_sandbox(
	    std::string name,
	    std::size_t memory_limit,
	    std::uint64_t instruction_limit,
	    logger log,
	    module_loader load_module,
	    void *owner) :
	    m_name(std::move(name)),
	    m_allocator{0, memory_limit},
	    m_instruction_limit(instruction_limit),
	    m_log(std::move(log)),
	    m_load_module(std::move(load_module)),
	    m_owner(owner)
	{
		ensure_lua_fork_sentinels();
		m_state = lua_newstate(allocate, &m_allocator);
		if (!m_state)
			throw std::runtime_error("could not create Lua sandbox for " + m_name);
		install_standard_library();
		lua_pushlightuserdata(m_state, &sandbox_registry_key);
		lua_pushlightuserdata(m_state, this);
		lua_rawset(m_state, LUA_REGISTRYINDEX);
		lua_pushlightuserdata(m_state, &loaded_modules_registry_key);
		lua_newtable(m_state);
		lua_rawset(m_state, LUA_REGISTRYINDEX);
	}

	lua_sandbox::~lua_sandbox()
	{
		if (m_state)
			lua_close(m_state);
	}

	lua_State *lua_sandbox::state() const noexcept { return m_state; }
	void *lua_sandbox::owner() const noexcept { return m_owner; }
	const std::string &lua_sandbox::name() const noexcept { return m_name; }
	std::size_t lua_sandbox::memory_used() const noexcept { return m_allocator.used; }

	bool lua_sandbox::execute(
	    std::string_view source,
	    std::string_view chunk_name,
	    std::string &error)
	{
		if (!source.empty()
		    && static_cast<unsigned char>(source.front()) == 0x1b)
		{
			error = "precompiled Lua bytecode is not accepted";
			return false;
		}
		const std::string name(chunk_name);
		if (luaL_loadbuffer(
		        m_state, source.data(), source.size(), name.c_str()) != 0)
		{
			error = lua_tostring(m_state, -1);
			lua_pop(m_state, 1);
			return false;
		}
		return protected_call(0, 0, error);
	}

	bool lua_sandbox::call(
	    int registry_reference,
	    const std::function<int(lua_State *)> &push_arguments,
	    int result_count,
	    std::string &error)
	{
		const auto original_top = lua_gettop(m_state);
		lua_rawgeti(m_state, LUA_REGISTRYINDEX, registry_reference);
		if (!lua_isfunction(m_state, -1))
		{
			lua_pop(m_state, 1);
			error = "Lua callback is no longer valid";
			return false;
		}
		int arguments{};
		try
		{
			arguments = push_arguments ? push_arguments(m_state) : 0;
		}
		catch (const std::exception &exception)
		{
			lua_settop(m_state, original_top);
			error = exception.what();
			return false;
		}
		catch (...)
		{
			lua_settop(m_state, original_top);
			error = "failed to prepare Lua callback arguments";
			return false;
		}
		return protected_call(arguments, result_count, error);
	}

	int lua_sandbox::reference_function(int index)
	{
		if (!lua_isfunction(m_state, index))
			throw std::invalid_argument("Lua callback must be a function");
		lua_pushvalue(m_state, index);
		return luaL_ref(m_state, LUA_REGISTRYINDEX);
	}

	void lua_sandbox::release(int registry_reference) noexcept
	{
		if (m_state && registry_reference != LUA_NOREF
		    && registry_reference != LUA_REFNIL)
			luaL_unref(m_state, LUA_REGISTRYINDEX, registry_reference);
	}

	void lua_sandbox::log(std::string message) const
	{
		if (m_log)
			m_log("[" + m_name + "] " + std::move(message));
	}

	lua_sandbox *lua_sandbox::from(lua_State *state) noexcept
	{
		lua_pushlightuserdata(state, &sandbox_registry_key);
		lua_rawget(state, LUA_REGISTRYINDEX);
		auto *result = static_cast<lua_sandbox *>(lua_touserdata(state, -1));
		lua_pop(state, 1);
		return result;
	}

	nlohmann::json lua_sandbox::to_json(
	    lua_State *state,
	    int index,
	    std::size_t maximum_bytes)
	{
		std::size_t items{};
		auto result = to_json_impl(state, index, 0, items);
		if (result.dump().size() > maximum_bytes)
			throw std::runtime_error("Lua JSON payload is too large");
		return result;
	}

	nlohmann::json lua_sandbox::parse_json(
	    std::string_view text,
	    std::size_t maximum_bytes)
	{
		if (text.empty() || text.size() > maximum_bytes)
			throw std::runtime_error("JSON payload size is invalid");
		std::size_t depth{};
		bool quoted{};
		bool escaped{};
		for (const char character : text)
		{
			if (quoted)
			{
				if (escaped)
					escaped = false;
				else if (character == '\\')
					escaped = true;
				else if (character == '"')
					quoted = false;
				continue;
			}
			if (character == '"')
				quoted = true;
			else if (character == '{' || character == '[')
			{
				if (++depth > 16)
					throw std::runtime_error("JSON nesting exceeds Lua limit");
			}
			else if ((character == '}' || character == ']') && depth > 0)
				--depth;
		}
		auto result = nlohmann::json::parse(text);
		validate_json(result);
		return result;
	}

	void lua_sandbox::validate_json(const nlohmann::json &value)
	{
		std::size_t items{};
		validate_json_impl(value, 0, items);
	}

	void lua_sandbox::push_json(lua_State *state, const nlohmann::json &value)
	{
		validate_json(value);
		push_json_impl(state, value);
	}

	void *lua_sandbox::allocate(
	    void *user,
	    void *pointer,
	    std::size_t old_size,
	    std::size_t new_size) noexcept
	{
		auto &state = *static_cast<allocator_state *>(user);
		if (!pointer)
		{
			if (new_size > state.limit - std::min(state.used, state.limit))
				return nullptr;
			void *result = std::malloc(new_size);
			if (result)
				state.used += new_size;
			return result;
		}
		if (new_size == 0)
		{
			state.used = old_size > state.used ? 0 : state.used - old_size;
			std::free(pointer);
			return nullptr;
		}
		if (new_size > old_size
		    && new_size - old_size > state.limit - std::min(state.used, state.limit))
			return nullptr;
		void *result = std::realloc(pointer, new_size);
		if (!result)
			return nullptr;
		state.used = new_size >= old_size
		    ? state.used + (new_size - old_size)
		    : state.used - std::min(state.used, old_size - new_size);
		return result;
	}

	void lua_sandbox::instruction_hook(lua_State *state, lua_Debug *)
	{
		auto *sandbox = from(state);
		if (!sandbox)
			return;
		sandbox->m_instruction_count += 1000;
		if (sandbox->m_instruction_count > sandbox->m_instruction_limit)
			luaL_error(state, "script instruction budget exceeded");
	}

	int lua_sandbox::require_module(lua_State *state)
	{
		auto *sandbox = from(state);
		const auto *name = luaL_checkstring(state, 1);
		const auto path = normalize_module(name ? name : "");
		if (!sandbox || path.empty() || !sandbox->m_load_module)
			return luaL_error(state, "invalid or unavailable module");

		lua_pushlightuserdata(state, &loaded_modules_registry_key);
		lua_rawget(state, LUA_REGISTRYINDEX);
		lua_getfield(state, -1, path.c_str());
		if (!lua_isnil(state, -1))
		{
			lua_remove(state, -2);
			return 1;
		}
		lua_pop(state, 1);
		const auto source = sandbox->m_load_module(path);
		if (!source)
		{
			lua_pop(state, 1);
			return luaL_error(state, "module '%s' was not found", name);
		}
		if (!source->empty()
		    && std::to_integer<unsigned char>(source->front()) == 0x1b)
		{
			lua_pop(state, 1);
			return luaL_error(state, "precompiled Lua bytecode is not accepted");
		}
		if (luaL_loadbuffer(
		        state,
		        reinterpret_cast<const char *>(source->data()),
		        source->size(), path.c_str()) != 0)
		{
			lua_remove(state, -2);
			return lua_error(state);
		}
		if (lua_pcall(state, 0, 1, 0) != 0)
		{
			lua_remove(state, -2);
			return lua_error(state);
		}
		if (lua_isnil(state, -1))
		{
			lua_pop(state, 1);
			lua_pushboolean(state, 1);
		}
		lua_pushvalue(state, -1);
		lua_setfield(state, -3, path.c_str());
		lua_remove(state, -2);
		return 1;
	}

	int lua_sandbox::print(lua_State *state)
	{
		auto *sandbox = from(state);
		std::string text;
		const auto count = lua_gettop(state);
		for (int index = 1; index <= count; ++index)
		{
			if (index > 1)
				text += '\t';
			if (lua_isboolean(state, index))
				text += lua_toboolean(state, index) ? "true" : "false";
			else if (lua_isnil(state, index))
				text += "nil";
			else if (lua_isstring(state, index) || lua_isnumber(state, index))
			{
				std::size_t length{};
				if (const auto *value = lua_tolstring(state, index, &length))
					text.append(value, length);
			}
			else
				text += lua_typename(state, lua_type(state, index));
		}
		if (sandbox)
			sandbox->log(std::move(text));
		return 0;
	}

	int lua_sandbox::traceback(lua_State *state)
	{
		if (!lua_isstring(state, 1))
			lua_pushliteral(state, "Lua error");
		else
			lua_pushvalue(state, 1);
		return 1;
	}

	void lua_sandbox::install_standard_library()
	{
		const struct
		{
			const char *name;
			lua_CFunction open;
		} libraries[] = {
		    {"", luaopen_base},
		    {LUA_TABLIBNAME, luaopen_table},
		    {LUA_STRLIBNAME, luaopen_string},
		    {LUA_MATHLIBNAME, luaopen_math}};
		for (const auto &library : libraries)
		{
			lua_pushcfunction(m_state, library.open);
			lua_pushstring(m_state, library.name);
			lua_call(m_state, 1, 0);
		}
		for (const auto *name : {
		         "dofile", "loadfile", "load", "loadstring", "module"})
		{
			lua_pushnil(m_state);
			lua_setglobal(m_state, name);
		}
		lua_pushcfunction(m_state, require_module);
		lua_setglobal(m_state, "require");
		lua_pushcfunction(m_state, print);
		lua_setglobal(m_state, "print");
	}

	bool lua_sandbox::protected_call(
	    int arguments,
	    int results,
	    std::string &error)
	{
		const auto function_index = lua_gettop(m_state) - arguments;
		lua_pushcfunction(m_state, traceback);
		lua_insert(m_state, function_index);
		m_instruction_count = 0;
		lua_sethook(m_state, instruction_hook, LUA_MASKCOUNT, 1000);
		const auto status = lua_pcall(
		    m_state, arguments, results, function_index);
		lua_sethook(m_state, nullptr, 0, 0);
		lua_remove(m_state, function_index);
		if (status == 0)
			return true;
		const auto *message = lua_tostring(m_state, -1);
		error = message ? message : "unknown Lua error";
		lua_pop(m_state, 1);
		return false;
	}
}
