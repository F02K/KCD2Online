#include "lua_manager_extension.hpp"

#include "bindings/gui_ext.hpp"
#include "bindings/kcd2.hpp"
#include "string/string.hpp"

#include <lua/lua_manager.hpp>

namespace big::lua_manager_extension
{
	void init_lua_manager(sol::state_view& state, sol::table& lua_ext, bool is_early_main)
	{
		init_lua_state(state, lua_ext, is_early_main);
		init_lua_api(state, lua_ext, is_early_main);
	}

	void init_lua_state(sol::state_view& state, sol::table& lua_ext, bool is_early_main)
	{
		if (is_early_main)
		{
			// clang-format off
			state.open_libraries(
			sol::lib::base,
			sol::lib::package,
			sol::lib::coroutine,
			sol::lib::string,
			sol::lib::os,
			sol::lib::math,
			sol::lib::table,
			sol::lib::debug,
			sol::lib::bit32,
			sol::lib::io,
			sol::lib::utf8);
			// clang-format on
		}
		else
		{
			// clang-format off
			state.open_libraries(
			sol::lib::os,
			sol::lib::io);
			// clang-format on
		}
	}

	static void set_folder_for_lua_require(folder& scripts_folder, sol::state_view& state)
	{
		std::string scripts_search_path  = state["package"]["path"];
		std::string scripts_folder_path  = (char*)scripts_folder.get_path().u8string().c_str();
		scripts_search_path             += ";" + scripts_folder_path + "/?.lua;";

		for (const auto& entry : std::filesystem::recursive_directory_iterator(scripts_folder.get_path(), std::filesystem::directory_options::skip_permission_denied))
		{
			if (!entry.is_directory())
			{
				continue;
			}

			std::string tmp      = (char*)entry.path().u8string().c_str();
			scripts_search_path += tmp + "/?.lua;";
		}
		// Remove final ';'
		scripts_search_path.pop_back();

		state["package"]["path"] = scripts_search_path;
	}

	void init_lua_api(sol::state_view& state, sol::table& lua_ext, bool is_early_main)
	{
		auto plugins_folder = big::g_file_manager.get_project_folder("plugins");
		set_folder_for_lua_require(plugins_folder, state);

		// Lua API: Field
		// Table: core
		// Field: is_early_main: boolean
		// Whether or not the early main Lua VM is running.
		// This Lua VM is there to allow Lua code to be injected into the game process before the CryEngine vanilla game Lua VM is init.
		// Some KCD2Online Lua api functions need to be executed under this early VM to do anything significant.
		// These functions are documented when such usage is needed and there are examples in the TestMod example mod files.
		lua_ext.create_named("core").set("is_early_main", is_early_main);

		auto on_import_table = lua_ext.create_named("on_import");

		// Lua API: Function
		// Table: on_import
		// Name: pre
		// Param: function: function: signature (string file_name, current_ENV_for_this_import) return nil or _ENV
		// The passed function will be called before the game loads a .lua script from the vanilla game.
		// The _ENV returned (if not nil) by the passed function gives you a way to define the _ENV of this lua script.
		on_import_table.set_function("pre",
		                             [](sol::protected_function f, sol::this_environment env)
		                             {
			                             auto mod = (lua_module_ext*)lua_module::this_from(env);
			                             if (mod)
			                             {
				                             mod->m_data_ext.m_on_pre_import.push_back(f);
			                             }
		                             });

		// Lua API: Function
		// Table: on_import
		// Name: post
		// Param: function: function: signature (string file_name)
		// The passed function will be called after the game loads a .lua script from the vanilla game.
		on_import_table.set_function("post",
		                             [](sol::protected_function f, sol::this_environment env)
		                             {
			                             auto mod = (lua_module_ext*)lua_module::this_from(env);
			                             if (mod)
			                             {
				                             mod->m_data_ext.m_on_post_import.push_back(f);
			                             }
		                             });


		// Let's keep that list sorted the same as the solution file explorer
		lua::gui_ext::bind(lua_ext);
		lua::kcd2::bind(lua_ext);
	}
} // namespace big::lua_manager_extension
