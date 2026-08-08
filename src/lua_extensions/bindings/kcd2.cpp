#include "kcd2.hpp"

#include <hooks/hooking.hpp>
#include <kcd2_address.hpp>
#include <kcd2_init.hpp>
#include <lua/lua_manager.hpp>
#include <lua_extensions/lua_module_ext.hpp>

#include <tuple>

namespace big
{
	extern toml_v2::config_file::config_entry<bool>* g_hook_log_write_enabled;
	extern ankerl::unordered_dense::set<std::string> g_fmod_events;
	extern uintptr_t g_CCryPak;
} // namespace big

namespace lua::kcd2
{
	static ankerl::unordered_dense::map<std::string, void*> g_file_path_to_bank;

	// Lua API: Function
	// Table: audio
	// Name: load_bank
	// Param: file_path: string: Path to the fmod .bank to load
	// Returns: bool: Returns true if bank loaded successfully.
	// The game currently use FMod Studio `2.02.21`. You can query the version by clicking checking Properties -> Details of the game `fmodstudio.dll`.
	// If your sound events correcty play but nothing can be heard, make sure that the guid of the Mixer masterBus, MixerInput output and MixerMaster id matches one from the game
	// You'll want to string replace the guids in the (at minimum 2) .xml files Master, Mixer, and any Metadata/Event events files that were made before the guid setup change
	static bool load_bank(const std::string& file_path, sol::this_environment env)
	{
		void* bank = nullptr;
		const auto res = big::fmodstudio_loadbankfile_orig(big::g_fmodstudio_instance, file_path.c_str(), 0x00'00'00'00 | 0x00'00'00'04, &bank);

		if (res)
		{
			LOG(ERROR) << "Failed loading fmod bank, fmod error code: " << res;

			return false;
		}

		g_file_path_to_bank[file_path] = bank;

		return true;
	}

	// Lua API: Function
	// Table: audio
	// Name: unload_bank
	// Param: file_path: string: Path to the fmod .bank to load
	// Returns: bool: Returns true if bank unloaded successfully.
	static bool unload_bank(const std::string& file_path, sol::this_environment env)
	{
		const auto it = g_file_path_to_bank.find(file_path);
		if (it == g_file_path_to_bank.end())
		{
			LOG(ERROR) << "Failed unloading fmod bank, bank doesn't exist";

			return false;
		}

		const auto res = big::fmodstudio_bank_unload_orig(it->second);

		if (res)
		{
			LOG(ERROR) << "Failed unloading fmod bank, fmod error code: " << res;

			return false;
		}

		return true;
	}

	static char CCryPak_OpenPacks_definition(__int64 a1, const char* szBindRoot, const char* pWildcardIn, int nFlags, __int64 pFullPaths)
	{
	}

	static __int64 __fastcall hook_mod_init_after_stream_engine_init(__int64 a1, unsigned __int64 a2)
	{
		const auto res = big::g_hooking->get_original<hook_mod_init_after_stream_engine_init>()(a1, a2);

		using namespace big;
		std::scoped_lock guard(g_lua_manager->m_module_lock);

		for (const auto& mod_ : g_lua_manager->m_modules)
		{
			auto mod = (lua_module_ext*)mod_.get();
			for (const auto& cb : mod->m_data_ext.m_on_pak_openable)
			{
				cb();
			}
		}

		return res;
	}

	void bind(sol::table& state)
	{
		{
			auto ns = state["log"].get_or_create<sol::table>();

			// Lua API: Function
			// Table: log
			// Name: set_output_vanilla_game_log
			// Param: new_value
			// Pass true to include vanilla game log messages in the console and file log output.
			ns["set_output_vanilla_game_log"] = [](bool new_value)
			{
				big::g_hook_log_write_enabled->set_value(new_value);
			};
		}

		{
			auto ns = state.create_named("game_data");

			// Lua API: Function
			// Table: game_data
			// Name: on_xml_parse
			// Param: function: function: signature (string file_name, string file_content) return new_file_content or nil
			// The passed function will be called before the game parses a .cfg file.
			// is_early_main setup/check required for some of the files.
			ns["on_xml_parse"] = [](sol::protected_function f, sol::this_environment env)
			{
				auto mod = (big::lua_module_ext*)big::lua_module::this_from(env);
				if (mod)
				{
					mod->m_data_ext.m_on_xml_parse.push_back(f);
				}
			};

			// Lua API: Function
			// Table: game_data
			// Name: on_cryfile_open
			// Param: old_filename: string: old filename that should get replaced
			// Param: new_filename: string: new filename that will replace the old one.
			// When the game opens a CryFile (Which can be a lot of different kind of files: .pak, .xml, .cfg and so on),
			// you can tell KCD2Online to instead load a certain new file instead.
			// is_early_main setup/check required for some of the files.
			ns["on_cryfile_open"] = [](const std::string& old_filename, const std::string& new_filename, sol::this_environment env)
			{
				auto mod = (big::lua_module_ext*)big::lua_module::this_from(env);
				if (mod)
				{
					mod->m_data_ext.m_cryfile_open_replace_map[old_filename] = new_filename;
					LOG(INFO) << "Replacing " << old_filename << " with " << new_filename;
				}
			};

			// Lua API: Function
			// Table: game_data
			// Name: open_pak
			// Param: root: string: root used for the pak file, game uses `data`, `localization`...
			// Param: path: string: file path to the .pak file to be opened.
			// To be used with `on_pak_openable`.
			// is_early_main setup/check required.
			ns["open_pak"] = [](const std::string& root, const std::string& path, sol::this_environment env)
			{
				static auto game_func = kcd2_address::scan("40 53 55 56 57 B8 ? ? ? ? E8 ? ? ? ? 48 2B E0 48 8B 05 ? ? "
				                                           "? ? 48 33 C4 48 89 84 24 ? ? ? ? 48 8B 01")
				                            .as_func<decltype(CCryPak_OpenPacks_definition)>();

				if (game_func && big::g_CCryPak)
				{
					// Usage found in the "Opening paks" string function (vanilla mod loader)
					game_func(big::g_CCryPak, root.c_str(), path.c_str(), 0x1'04'00LL, 0);
				}
			};

			// Lua API: Function
			// Table: game_data
			// Name: on_pak_openable
			// Param: function: function: signature () return void / nil
			// The passed function will be called when the game is loading mods .pak files
			// is_early_main setup/check is required.
			ns["on_pak_openable"] = [](sol::protected_function f, sol::this_environment env)
			{
				auto mod = (big::lua_module_ext*)big::lua_module::this_from(env);
				if (mod)
				{
					mod->m_data_ext.m_on_pak_openable.push_back(f);
				}
			};
		}

		{
			static auto game_func = kcd2_address::scan("E8 ? ? ? ? 48 8D 4C 24 ? E8 ? ? ? ? 48 8B 8E ? ? ? ? E8");
			if (game_func)
			{
				big::hooking::detour_hook_helper::add<hook_mod_init_after_stream_engine_init>(
				    "hook_mod_init_after_stream_engine_init",
				    game_func.get_call());
			}
			else
			{
				LOG(ERROR) << "failed hook_mod_init_after_stream_engine_init";
			}
		}

		{
			auto ns = state.create_named("audio");

			ns.set_function("load_bank", load_bank);
			ns.set_function("unload_bank", unload_bank);

			// Lua API: Function
			// Table: audio
			// Name: on_fmod_getevent
			// Param: function: function: signature (string event_name) return new_event_name or nil
			// The passed function will be called before the game loads a fmod event. is_early_main setup/check required.
			ns["on_fmod_getevent"] = [](sol::protected_function f, sol::this_environment env)
			{
				auto mod = (big::lua_module_ext*)big::lua_module::this_from(env);
				if (mod)
				{
					mod->m_data_ext.m_on_fmod_getevent.push_back(f);
				}
			};

			// Lua API: Field
			// Table: audio
			// Field: events: table<string>
			// All fmod string events used by the game so far.
			ns["events"] = sol::as_table(std::ref(big::g_fmod_events));
		}

		{
			auto ns = state.create_named("cryengine");

			// Lua API: Function
			// Table: cryengine
			// Name: on_lua_userdata_bind
			// Param: function: function: signature (string cscriptablebase_name, table binded_script_table, table binded_metatable)
			// The passed function will be called when a cryengine lua userdata is bound.
			ns["on_lua_userdata_bind"] = [](sol::protected_function f, sol::this_environment env)
			{
				auto mod = (big::lua_module_ext*)big::lua_module::this_from(env);
				if (mod)
				{
					mod->m_data_ext.m_on_cryengine_lua_userdata_bind.push_back(f);
				}
			};
		}
	}
} // namespace lua::kcd2
