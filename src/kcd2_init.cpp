#include "kcd2_init.hpp"

#include "engine_path.hpp"
#include "hooks/hooking.hpp"
#include "kcd2_address.hpp"
#include "memory/gm_address.hpp"

#include <config/config.hpp>
#include <gui/gui.hpp>
#include <gui/ingame_chat.hpp>
#include <gui/native_multiplayer_menu.hpp>
#include <gui/renderer.hpp>
#include <kcse/client_proxy.hpp>
#include <logger/stack_trace.hpp>
#include <lua_extensions/lua_manager_extension.hpp>
#include <pugixml.hpp>
#include <threads/thread_pool.hpp>

extern "C"
{
#include <ldo.h>
#include <ltable.h>
#include <lua.h>
#include <zip.h>
#undef lua_load
	extern void luaV_execute(lua_State *L, int nexeccalls);
}

namespace big
{
	static lua_State *g_early_main_lua_state = nullptr;

	extern void render_imgui_frame();
	static void execute_queued_engine_console_commands();

	static bool lua_init_once = true;

	// Currently this is not the lua init function, it's the CryScriptSystem::Update function
	static __int64 __fastcall hook_CScriptSystem_Update(__int64 a1)
	{
		std::scoped_lock l(lua_manager_extension::g_manager_mutex);
		std::scoped_lock l2(g_lua_manager->m_module_lock);

		const auto res = big::g_hooking->get_original<hook_CScriptSystem_Update>()(a1);
		execute_queued_engine_console_commands();

		if (g_lua_execute_buffer_queue.size())
		{
			auto state_view = sol::state_view(g_lua_manager->lua_state());
			for (const auto &buffer : g_lua_execute_buffer_queue)
			{
				try
				{
					state_view.script(buffer);
				}
				catch (const std::exception &e)
				{
					LOG(ERROR) << e.what();
				}
			}
			g_lua_execute_buffer_queue.clear();
		}

		render_imgui_frame();
		native_multiplayer_menu::update();

		lua_manager_extension::g_lua_manager_instance->process_file_watcher_queue();

		return res;
	}

	//static __int64 __fastcall hook_Initializing_Direct3D(__int64 a1, __int64 a2, __int64 a3, unsigned int a4, int a5, int a6, int a7, int a8, __int64 a9)
	static __int64 __fastcall hook_Initializing_Direct3D()
	{
		LOG(INFO) << "Initializing_Direct3D called (currently sys_flash_address_space setter function)";

		const auto res = big::g_hooking->get_original<hook_Initializing_Direct3D>()();

		//std::thread(
		//[]()
		//{
		big::g_renderer->hook();
		g_running = true;
		//})
		//.detach();

		LOG(INFO) << "Initializing_Direct3D finished";

		return res;
	}

	static kcd2_address game_lua_call;

	static void hook_lua_call(lua_State *L, int nargs, int nresults)
	{
		std::scoped_lock l(lua_manager_extension::g_manager_mutex);
		std::scoped_lock l2(g_lua_manager->m_module_lock);

		return game_lua_call.as_func<decltype(lua_call)>()(L, nargs, nresults);
	}

	static kcd2_address game_lua_checkstack;

	static int hook_lua_checkstack(lua_State *L, int extra)
	{
		return game_lua_checkstack.as_func<decltype(lua_checkstack)>()(L, extra);
	}

	static kcd2_address game_lua_concat;

	static void hook_lua_concat(lua_State *L, int n)
	{
		return game_lua_concat.as_func<decltype(lua_concat)>()(L, n);
	}

	static kcd2_address game_lua_createtable;

	static void hook_lua_createtable(lua_State *L, int narr, int nrec)
	{
		return game_lua_createtable.as_func<decltype(lua_createtable)>()(L, narr, nrec);
	}

	static kcd2_address game_lua_error;

	static int hook_lua_error(lua_State *L)
	{
		return game_lua_error.as_func<decltype(lua_error)>()(L);
	}

	static kcd2_address game_lua_gc;

	static int hook_lua_gc(lua_State *L, int what, int data)
	{
		return game_lua_gc.as_func<decltype(lua_gc)>()(L, what, data);
	}

	static kcd2_address game_lua_getfenv;

	static void hook_lua_getfenv(lua_State *L, int index)
	{
		return game_lua_getfenv.as_func<decltype(lua_getfenv)>()(L, index);
	}

	static kcd2_address game_lua_getfield;

	static void hook_lua_getfield(lua_State *L, int index, const char *k)
	{
		return game_lua_getfield.as_func<decltype(lua_getfield)>()(L, index, k);
	}

	static kcd2_address game_lua_getmetatable;

	static int hook_lua_getmetatable(lua_State *L, int index)
	{
		return game_lua_getmetatable.as_func<decltype(lua_getmetatable)>()(L, index);
	}

	static kcd2_address game_lua_gettable;

	static void hook_lua_gettable(lua_State *L, int index)
	{
		return game_lua_gettable.as_func<decltype(lua_gettable)>()(L, index);
	}

	static kcd2_address game_lua_insert;

	static void hook_lua_insert(lua_State *L, int index)
	{
		return game_lua_insert.as_func<decltype(lua_insert)>()(L, index);
	}

	static sol::optional<sol::environment> env_to_add;
	static kcd2_address game_lua_pcall, game_lua_setmetatable, CScriptableBase_Init_func, lua_custom_alloc;

	//static ankerl::unordered_dense::set<size_t> g_lua_different_threads;

	static int hook_game_lua_pcall(lua_State *L, int nargs, int nresults, int errfunc)
	{
		std::scoped_lock l(lua_manager_extension::g_manager_mutex);
		std::scoped_lock l2(g_lua_manager->m_module_lock);

		if (env_to_add.has_value() && env_to_add.value().valid())
		{
			sol::set_environment(env_to_add.value(), sol::stack_reference(L, -1));
		}

		//auto current_thread_id = GetCurrentThreadId();
		//if (!g_lua_different_threads.contains(current_thread_id))
		//{
		//g_lua_different_threads.insert(current_thread_id);
		//LOG(ERROR) << "lua_pcall current thread id: " << current_thread_id;
		//}

		return big::g_hooking->get_original<hook_game_lua_pcall>()(L, nargs, nresults, errfunc);
	}

	static int hook_lua_pcall(lua_State *L, int nargs, int nresults, int errfunc)
	{
		return game_lua_pcall.as_func<decltype(lua_pcall)>()(L, nargs, nresults, errfunc);
	}

	static kcd2_address game_luaV_execute;

	static void hook_luaV_execute(lua_State *L, int nexeccalls)
	{
		//std::scoped_lock l(lua_manager_extension::g_manager_mutex);

		return game_luaV_execute.as_func<decltype(luaV_execute)>()(L, nexeccalls);
	}

	static kcd2_address game_lua_load;

	static int hook_lua_load(lua_State *L, lua_Reader reader, void *dt, const char *chunkname)
	{
		//std::scoped_lock l(lua_manager_extension::g_manager_mutex);

		return game_lua_load.as_func<decltype(lua_load)>()(L, reader, dt, chunkname);
	}

	static void CScriptTable_PushRef_def(void *a1, void *a2)
	{
	}

	static ankerl::unordered_dense::set<void *> g_lua_game_metatables;

	class vtable_helper
	{
	public:
		virtual ~vtable_helper()
		{
		}
	};

	struct CScriptableBase_info
	{
		void *m_ptr;
		std::string m_typeid_name;
	};

	static ankerl::unordered_dense::map<void *, CScriptableBase_info> g_metatable_ptr_to_CScriptable;
	static kcd2_address game_pushref;

	static void hook_CScriptTable_SetMetatable(void *this_, void *pMetatable)
	{
		big::g_hooking->get_original<hook_CScriptTable_SetMetatable>()(this_, pMetatable);

		static auto PushRef = game_pushref.as_func<decltype(CScriptTable_PushRef_def)>();

		auto L = g_lua_manager->lua_state();
		sol::state_view state_view(L);

		const auto &cscriptable_info = g_metatable_ptr_to_CScriptable[pMetatable];

		std::string key;
		if (!cscriptable_info.m_typeid_name.empty())
		{
			key = cscriptable_info.m_typeid_name;
		}
		else
		{
			static uint64_t unk_counter = 0;
			key                         = std::format("Unknown Metatable {}", unk_counter++);
		}

		PushRef(this_, this_);
		const auto table_entry = sol::stack::get<sol::table>(state_view, -1);
		lua_pop(L, 1);
		PushRef(this_, pMetatable);
		const auto metatable_entry = sol::stack::get<sol::table>(state_view, -1);
		lua_pop(L, 1);
		std::scoped_lock guard(big::g_lua_manager->m_module_lock);
		for (const auto &mod_ : big::g_lua_manager->m_modules)
		{
			auto mod = (lua_module_ext *)mod_.get();
			for (const auto &cb : mod->m_data_ext.m_on_cryengine_lua_userdata_bind)
			{
				cb(key, table_entry, metatable_entry);
			}
		}

		if (g_lua_game_metatables.contains(pMetatable))
		{
			return;
		}

		g_lua_game_metatables.insert(pMetatable);
	}

	void hook_CScriptableBase_Init(__int64 a1, __int64 IScriptSystem_pSS, __int64 ISystem_pSystem, int nParamIdOffset)
	{
		big::g_hooking->get_original<hook_CScriptableBase_Init>()(a1, IScriptSystem_pSS, ISystem_pSystem, nParamIdOffset);

		g_ISystem = ISystem_pSystem;

		const auto metatable = *(__int64 *)(a1 + 72);
		if (metatable)
		{
			vtable_helper *vth = (vtable_helper *)a1;

			g_metatable_ptr_to_CScriptable[(void *)metatable] = {(void *)a1, typeid(*vth).name()};
		}
	}

	static void *hook_lua_mimalloc(void *ud, void *ptr, size_t osize, size_t nsize)
	{
		(void)ud;
		(void)osize;
		if (nsize == 0)
		{
			mi_free(ptr);
			return NULL;
		}
		else
		{
			return mi_realloc_aligned(ptr, nsize, 16);
		}
	}

	static char hook_CryScriptSystem_Init(void *this_, __int64 a2)
	{
		std::scoped_lock l(lua_manager_extension::g_manager_mutex);

		const auto early_main_state = lua_manager_extension::g_lua_manager_instance->lua_state();
		lua_manager_extension::g_lua_manager_instance.reset();
		LOG(INFO) << "Lua manager early main reset";
		lua_close(early_main_state);

		LOG(INFO) << "Starting hook queue";

		// no idea if all these hook are necessary, the one that did the trick was lua_load i'm pretty sure
		big::hooking::detour_hook_helper::add_queue<hook_luaV_execute>("", &luaV_execute);
		big::hooking::detour_hook_helper::add_queue<hook_lua_call>("", &lua_call);
		big::hooking::detour_hook_helper::add_queue<hook_lua_checkstack>("", &lua_checkstack);
		big::hooking::detour_hook_helper::add_queue<hook_lua_createtable>("", &lua_createtable);
		big::hooking::detour_hook_helper::add_queue<hook_lua_error>("", &lua_error);
		big::hooking::detour_hook_helper::add_queue<hook_lua_gc>("", &lua_gc);
		big::hooking::detour_hook_helper::add_queue<hook_lua_getfenv>("", &lua_getfenv);
		big::hooking::detour_hook_helper::add_queue<hook_lua_getfield>("", &lua_getfield);
		big::hooking::detour_hook_helper::add_queue<hook_lua_getmetatable>("", &lua_getmetatable);
		big::hooking::detour_hook_helper::add_queue<hook_lua_gettable>("", &lua_gettable);
		big::hooking::detour_hook_helper::add_queue<hook_lua_insert>("", &lua_insert);
		big::hooking::detour_hook_helper::add_queue<hook_game_lua_pcall>("", game_lua_pcall);
		big::hooking::detour_hook_helper::add_queue<hook_lua_pcall>("", &lua_pcall);
		big::hooking::detour_hook_helper::add_queue<hook_lua_load>("lua_load_hook", &lua_load);
		big::hooking::detour_hook_helper::add_queue<hook_CScriptTable_SetMetatable>("hook_CScriptTable_SetMetatable", game_lua_setmetatable);
		big::hooking::detour_hook_helper::add_queue<hook_CScriptableBase_Init>("hook_CScriptableBase_Init", CScriptableBase_Init_func);
		big::hooking::detour_hook_helper::add_queue<hook_lua_mimalloc>("hook_lua_custom_alloc", lua_custom_alloc);
		big::hooking::detour_hook_helper::execute_queue();

		LOG(INFO) << "Ending hook queue";

		const auto res = big::g_hooking->get_original<hook_CryScriptSystem_Init>()(this_, a2);

		return res;
	}

	static void *__fastcall hook_attachVariable(void *this_, const char *szVarName, int *pContainer, const char *szComment, int dwFlags)
	{
		if (strcmp(szVarName, "sys_PakPriority") == 0)
		{
			LOG(INFO) << "sys_PakPriority intercepted. Setting to 0.";
			*pContainer = 0;
		}

		const auto res = big::g_hooking->get_original<hook_attachVariable>()(this_, szVarName, pContainer, szComment, dwFlags);

		return res;
	}

	static void hook_PostInputEvent(char **a1, __int64 a2, __int64 a3, __int64 a4)
	{
		if (native_multiplayer_menu::blocks_game_input()
		    || ingame_chat::blocks_game_input())
		{
			return;
		}
		if (big::g_gui && big::g_gui->is_open() && !big::g_gui->let_game_input_go_through_gui_layer)
		{
			return;
		}

		big::g_hooking->get_original<hook_PostInputEvent>()(a1, a2, a3, a4);
	}

	static void __fastcall hook_C_UIMenu_ShowPage(void *menu)
	{
		native_multiplayer_menu::before_show_page(menu);
		big::g_hooking->get_original<hook_C_UIMenu_ShowPage>()(menu);
	}

	static void __fastcall hook_C_SkipTimeCutscene_Play(void *cutscene)
	{
		const auto status = kcd2o::kcse::ui_client().status();
		if (status.state == kcd2o::client_state::connected)
		{
			LOG(INFO) << "Blocked vanilla skip time while multiplayer is connected.";
			return;
		}
		big::g_hooking->get_original<hook_C_SkipTimeCutscene_Play>()(
		    cutscene);
	}

	static char __fastcall hook_C_FastTravel_StartTravel(void *fast_travel)
	{
		if (kcd2o::kcse::ui_client().status().state
		    == kcd2o::client_state::connected)
			return 0;
		return big::g_hooking
		    ->get_original<hook_C_FastTravel_StartTravel>()(fast_travel);
	}

	enum CryEngine_ELogType
	{
		eMessage,
		eWarning,
		eError,
		eAlways,
		eWarningAlways,
		eErrorAlways,
		eInput,         //!< e.g. "e_CaptureFolder ?" or all printouts from history or auto completion.
		eInputResponse, //!< e.g. "Set output folder for video capturing" in response to "e_CaptureFolder ?".
		eComment,
		eAssert
	};

	toml_v2::config_file::config_entry<bool> *g_hook_log_write_enabled = nullptr;

	static void __fastcall hook_CLog_LogV(__int64 this_, __int64 a2, unsigned int a3, CryEngine_ELogType elogtype, int a5, const char *szFormat, va_list args)
	{
		if (szFormat && g_hook_log_write_enabled->get_value())
		{
			va_list args_copy;
			va_copy(args_copy, args);

			// Determine required buffer size (excluding null terminator)
			int required_size = vsnprintf(nullptr, 0, szFormat, args_copy);
			va_end(args_copy);

			if (required_size > 0) // Ensure formatting succeeded
			{
				// Stack allocation using alloca (adds +1 for null terminator)
				char *buffer = (char *)alloca(required_size + 1);

				va_copy(args_copy, args);
				vsnprintf(buffer, required_size + 1, szFormat, args_copy);
				va_end(args_copy);

				switch (elogtype)
				{
				case eMessage:       LOG(INFO) << buffer; break;
				case eWarning:       LOG(WARNING) << buffer; break;
				case eError:         LOG(ERROR) << buffer; break;
				case eAlways:        LOG(INFO) << buffer; break;
				case eWarningAlways: LOG(WARNING) << buffer; break;
				case eErrorAlways:   LOG(ERROR) << buffer; break;
				case eInput:         LOG(INFO) << buffer; break;
				case eInputResponse: LOG(INFO) << buffer; break;
				case eComment:       LOG(INFO) << buffer; break;
				case eAssert:        LOG(ERROR) << "[Assert] " << buffer; break;
				default:             LOG(INFO) << buffer; break;
				}
			}
		}

		big::g_hooking->get_original<hook_CLog_LogV>()(this_, a2, a3, elogtype, a5, szFormat, args);
	}

	static kcd2_address game_index2adr;
	static kcd2_address game_luaH_new;

	bool __fastcall hook_CScriptSystem_ExecuteBuffer(__int64 this_, const char *sBuffer, __int64 size, const char *sBufferDescription, __int64 pEnvironmentLua)
	{
		std::scoped_lock l(lua_manager_extension::g_manager_mutex);

		if (lua_init_once)
		{
			lua_init_once = false;

			lua_State *L = *reinterpret_cast<lua_State **>(this_ + 16);

			// must ensure dummynode / luaO_nilobject from the game WHGame.dll is used and not ours.
			{
				static auto __index2adr         = game_index2adr.as_func<intptr_t(lua_State *, int)>();
				luaO_nilobject_external_address = __index2adr(L, 999'999);
				static auto __luaH_new          = game_luaH_new.as_func<Table *(lua_State *, int, int)>();
				auto game_table                 = __luaH_new(L, 0, 0);
				dummynode_external_address      = (intptr_t)game_table->node;
			}

			// https://github.com/lp249839965/CryEngine-5.2.3/blob/ef4f45fe2ff05ad788fec1dd6ac56c038731e29d/Code/CryEngine/CryScriptSystem/ScriptSystem.cpp#L747
			{
				L->l_G->storedebug = 1;
			}

			LOG(INFO) << "Lua manager init";
			lua_manager_extension::g_lua_manager_instance = std::make_unique<lua_manager>(
			    L,
			    g_file_manager.get_project_folder("config"),
			    g_file_manager.get_project_folder("plugins_data"),
			    g_file_manager.get_project_folder("plugins"),
			    [](sol::state_view &state, sol::table &lua_ext)
			    {
				    lua_manager_extension::init_lua_manager(state, lua_ext, false);
			    },
			    [](sol::state_view &state) -> sol::environment
			    {
				    // Cleanup the Global table for ReturnOfModding lua mods.

				    // rom.game = _G
				    state[rom::g_lua_api_namespace]["game"] = state["_G"];

				    // local plugin_G = {}
				    sol::table plugin_G = state.create_table();

				    sol::table all_g = state["_G"];
				    for (const auto &[k, v] : all_g)
				    {
					    if (k.is<const char *>())
					    {
						    auto key_str = k.as<const char *>();
						    // Bad heuristic for filtering out native functions from the game code
						    // This heuristic was for hades 2
						    // todo: check for kcd2
						    if (!std::isupper(static_cast<unsigned char>(key_str[0])))
						    {
							    plugin_G[k] = v;
						    }
					    }
					    else
					    {
						    plugin_G[k] = v;
					    }
				    }

				    // plugin_G.rom = rom
				    plugin_G[rom::g_lua_api_namespace] = state[rom::g_lua_api_namespace];

				    // plugin_G._G = plugin_G
				    plugin_G["_G"] = plugin_G;

				    // when you give plugins an _ENV, plugin_G is the __index instead
				    return sol::environment(state, sol::create, plugin_G);
			    });

			lua_manager_extension::g_lua_manager_instance->init<lua_module_ext>(true);
		}

		std::scoped_lock guard(g_lua_manager->m_module_lock);

		for (const auto &mod_ : g_lua_manager->m_modules)
		{
			auto mod = (lua_module_ext *)mod_.get();
			for (const auto &cb : mod->m_data_ext.m_on_pre_import)
			{
				auto res        = cb(sBufferDescription, env_to_add.has_value() ? env_to_add.value() : sol::lua_nil);
				auto env_to_set = res.get<sol::optional<sol::environment>>();
				if (env_to_set && env_to_set.value() && env_to_set.value().valid())
				{
					env_to_add = env_to_set;
					LOG(INFO) << "Setting _ENV for this script to " << mod->guid();
				}
			}
		}

		const auto res = big::g_hooking->get_original<hook_CScriptSystem_ExecuteBuffer>()(this_, sBuffer, size, sBufferDescription, pEnvironmentLua);

		env_to_add = {};

		for (const auto &mod_ : g_lua_manager->m_modules)
		{
			auto mod = (lua_module_ext *)mod_.get();
			for (const auto &cb : mod->m_data_ext.m_on_post_import)
			{
				cb(sBufferDescription);
			}
		}

		return res;
	}

	__int64 hook_fmodstudio_loadbankfile(void *this_, const char *filename, __int64 fmod_studio_load_bank_flags, void **bank)
	{
		g_fmodstudio_instance = this_;

		return fmodstudio_loadbankfile_orig(this_, filename, fmod_studio_load_bank_flags, bank);
	}

	static __int64 hook_fmodstudio_getevent(void *fmodstudio_event_system_this, const char *event_name, void **event_description_result)
	{
		std::scoped_lock l(lua_manager_extension::g_manager_mutex);

		std::string new_event_name = event_name;

		if (g_lua_manager)
		{
			std::scoped_lock guard(g_lua_manager->m_module_lock);

			for (const auto &mod_ : g_lua_manager->m_modules)
			{
				auto mod = (lua_module_ext *)mod_.get();
				for (const auto &cb : mod->m_data_ext.m_on_fmod_getevent)
				{
					auto res = cb(new_event_name);
					if (res.get_type() == sol::type::string)
					{
						new_event_name = res.get<std::string>();
					}
				}
			}
		}

		g_fmod_events.insert(new_event_name.c_str());

		const auto res = fmodstudio_getevent_orig(fmodstudio_event_system_this, new_event_name.c_str(), event_description_result);
		if (res != 0)
		{
			LOG(WARNING) << "Failed playing " << new_event_name.c_str() << " - " << res << " - " << event_description_result;
		}

		return res;
	}

	const char *g_xml_current_parsed_filename = nullptr;

	static __int64 *__fastcall hook_XmlParserImp_ParseFile(void *this_, __int64 *a2, const char *filename, char a4)
	{
		g_xml_current_parsed_filename = filename;

		const auto res = big::g_hooking->get_original<hook_XmlParserImp_ParseFile>()(this_, a2, filename, a4);

		g_xml_current_parsed_filename = nullptr;

		return res;
	}

	static std::string strip_last_double_underscore(const std::string &path)
	{
		size_t pos = path.rfind("__");
		if (pos != std::string::npos)
		{
			return path.substr(0, pos) + ".xml";
		}

		return path;
	}

	static std::string strip_last_file_separator(const std::string &path)
	{
		return engine_path::filename_bytes(path);
	}

	static std::string get_normalized_original_filename(const std::string &input)
	{
		return strip_last_file_separator(big::string::to_lower(input));
	}

	static std::string get_original_filename_from_mod_filename(const std::string &input)
	{
		return strip_last_file_separator(strip_last_double_underscore(big::string::to_lower(input)));
	}

	static std::string get_xml_context(const std::string &input)
	{
		const auto lowered_input = big::string::to_lower(input);

		//LOG(INFO) << lowered_input;

		const std::string levels_str = "levels/";
		if (lowered_input.contains(levels_str))
		{
			auto split1 = big::string::split(lowered_input, levels_str);
			if (split1.size() > 1)
			{
				if (split1[1].contains('/'))
				{
					auto split2 = big::string::split(split1[1], '/');

					//LOG(INFO) << "xml context: " << split2[0] << " from " << lowered_input;
					return split2[0];
				}

				//LOG(INFO) << "xml context: " << split1[1] << " from " << lowered_input;
				return split1[1];
			}
		}

		return "global";
	}

	void parse_xml_entity(pugi::xml_node in_node, const std::string &entity_class_name, xml_node_metadata_t &metadata_out_node, entity_xml_info_t *entity_out_info)
	{
		entity_out_info->m_id = metadata_out_node.m_id;

		for (const auto &attr : in_node.attributes())
		{
			const std::string attr_name = attr.name();
			if (attr_name.empty())
			{
				continue;
			}
			const std::string attr_value = attr.value();
			if (attr_value.empty())
			{
				continue;
			}

			auto &attribute_potential_values = metadata_out_node.m_attributes[attr_name];
			if (attribute_potential_values.size() < 20)
			{
				attribute_potential_values.insert(attr_value);
			}
			auto &attribute_entityclass = metadata_out_node.m_attributes_entityclass[attr_name];
			attribute_entityclass.insert(entity_class_name);

			entity_out_info->m_attributes.emplace_back(attr_name, attr_value);
		}

		for (const auto &child : in_node.children())
		{
			const std::string child_name = child.name();
			if (child_name.empty())
			{
				continue;
			}

			const auto metadata_id    = metadata_out_node.m_id + '_' + child_name;
			auto &child_node_metadata = g_entity_xml_metadata[metadata_id];
			if (!child_node_metadata.m_id.size())
			{
				child_node_metadata.m_id   = metadata_id;
				child_node_metadata.m_name = child_name;
			}
			metadata_out_node.m_childrens.insert(metadata_id);
			auto &childrens_entityclass = metadata_out_node.m_childrens_entityclass[metadata_id];
			childrens_entityclass.insert(entity_class_name);

			entity_xml_info_t *child_xml_info = new entity_xml_info_t;
			child_xml_info->m_name            = child_name;

			parse_xml_entity(child, entity_class_name, child_node_metadata, child_xml_info);

			entity_out_info->m_childrens.push_back(child_xml_info);
		}
	}

	void parse_xml_objects_file(const char *file_content_, size_t file_size_)
	{
		const std::string file_content(file_content_, file_size_);

		g_thread_pool->push(
		    [file_content]()
		    {
			    pugi::xml_document doc;
			    if (!doc.load_buffer(file_content.c_str(), file_content.size()))
			    //if (!doc.load_file("C:/Users/Quentin/Desktop/kcd2/level_kutnohorsko/objects_mission0.xml"))
			    {
				    LOG(ERROR) << "Failed to load <Objects> XML file";
				    return;
			    }

			    const std::string entity_str = "Entity";

			    std::scoped_lock l(g_xml_info_mutex);

			    for (auto &entity : doc.child("Objects").children(entity_str.c_str()))
			    {
				    const std::string entity_guid       = entity.attribute("EntityGuid").value();
				    const std::string entity_class_name = entity.attribute("EntityClass").value();

				    entity_xml_info_t *entity_xml_info   = new entity_xml_info_t;
				    entity_xml_info->m_name              = entity_str;
				    entity_xml_info->m_entity_name       = entity.attribute("Name").value();
				    entity_xml_info->m_entity_class_name = entity_class_name;

				    auto &entity_root_node = g_entity_xml_metadata[entity_str];
				    if (!entity_root_node.m_id.size())
				    {
					    entity_root_node.m_id   = entity_str;
					    entity_root_node.m_name = entity_str;
				    }
				    parse_xml_entity(entity, entity_class_name, entity_root_node, entity_xml_info);

				    g_entity_xml_infos.push_back(entity_xml_info);
				    g_entity_guid_to_xml_infos[entity_guid] = entity_xml_info;
			    }

			    //LOG(INFO) << "Parsed an XML file containing <Objects>";
		    });
	}

	static __int64 __fastcall hook_XML_Parse(__int64 parser, const char *pFileContents, int fileSize)
	{
		std::scoped_lock l(lua_manager_extension::g_manager_mutex);

		// Test code just below useful for finding xref across data, wh_data level files however doesn't seem to get parsed here?
		/*if (new_file_content.contains("a659399b-8517-4cc7") || new_file_content.contains("3a7fb2dc-9118-418a-bb5c-c8db913467ed")
		    || new_file_content.contains("763db0bb-4469-497d-bdc9-712b3df91b5a") || new_file_content.contains("ksta_additive_man_18"))
		{
			LOG(ERROR) << g_xml_current_parsed_filename ? g_xml_current_parsed_filename : "No filename";
		}*/

		// TODO: try to re-enable this, there is lua vm crashes because of this, due to it not being called from the lua script thread.
		//if (g_lua_manager)
		//{
		//std::scoped_lock guard(g_lua_manager->m_module_lock);

		//for (const auto &mod_ : g_lua_manager->m_modules)
		//{
		//auto mod = (lua_module_ext *)mod_.get();
		//for (const auto &cb : mod->m_data_ext.m_on_xml_parse)
		//{


		//auto res = cb(g_xml_current_parsed_filename ? g_xml_current_parsed_filename : "", new_file_content);
		//if (res.get_type() == sol::type::string)
		//{
		//new_file_content = res.get<std::string>();
		//}
		//}
		//}
		//}

		const char *current_parsed_filename = g_xml_current_parsed_filename;
		if (current_parsed_filename)
		{
			const auto original_filename = get_normalized_original_filename(current_parsed_filename);

			const auto xml_context = get_xml_context(current_parsed_filename);

			const auto it = g_xml_context_to_xml_filename_to_modifications[xml_context].find(original_filename);
			if (it != g_xml_context_to_xml_filename_to_modifications[xml_context].end())
			{
				// Engine paths are byte strings and are not guaranteed to be valid in
				// the active Windows code page. Avoid std::filesystem here because its
				// narrow path constructor performs a potentially throwing UTF-16
				// conversion.
				const auto &modded_xml_filename_lowered = original_filename;
				if (!g_xml_context_to_modded_xml_filenames[xml_context].contains(modded_xml_filename_lowered))
				{
					LOG(INFO) << "[XML Merger] Applying xml merge patches for " << original_filename << " (" << current_parsed_filename << " - " << modded_xml_filename_lowered << ") xml context: " << xml_context;

					std::string new_file_content;
					new_file_content.assign(pFileContents, fileSize);
					apply_xml_patches(new_file_content, it->second, original_filename.contains("inventorypreset"));

					if (big::string::starts_with("<Objects>", pFileContents))
					{
						//LOG(INFO) << "Found <Objects>";
						parse_xml_objects_file(new_file_content.c_str(), new_file_content.size());
					}

					return big::g_hooking->get_original<hook_XML_Parse>()(parser,
					                                                      new_file_content.c_str(),
					                                                      new_file_content.size());

					//if (!modded_xml_filename_lowered.contains("component"))
					//{
					//std::ofstream file("C:/Users/Quentin/Desktop/inventorypreset__shop_PATCHED.xml", std::ios::binary);
					//if (file)
					//{
					//file.write(new_file_content.data(), new_file_content.size());
					//}
					//}
				}
			}
			else
			{
				//LOG(INFO) << original_filename;
			}

			if (big::string::starts_with("<Objects>", pFileContents))
			{
				//LOG(INFO) << "Found <Objects>";
				parse_xml_objects_file(pFileContents, fileSize);
			}
		}


		return big::g_hooking->get_original<hook_XML_Parse>()(parser, pFileContents, fileSize);
	}

	uintptr_t g_CCryPak = 0;

	static __int64 __fastcall hook_CCryPak_ctor(__int64 a1, __int64 a2, void *a3)
	{
		g_CCryPak = a1;

		return big::g_hooking->get_original<hook_CCryPak_ctor>()(a1, a2, a3);
	}

	static const char *g_table_current_table_name = nullptr;
	static const char *g_table_current_mod_name   = nullptr;

	static bool is_safe_ptr(const char *ptr)
	{
		__try
		{
			// Attempt to read memory at ptr
			if (ptr && *ptr)
			{
				return ptr;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
		return false;
	}

	// Wrapper function that safely converts the string
	static std::string safe_string_convert(const char *ptr)
	{
		return is_safe_ptr(ptr) ? std::string(ptr) : std::to_string((uintptr_t)ptr);
	}

	// Original function returns true if xml is parsed successfully.
	// Hook: Returns true and skip orig call for skipping xml parsing of a given file
	static bool hook_XmlParserReadOnly_Read_caller(__int64 a1, const char **ptr_to_filename, char a3)
	{
		if (ptr_to_filename && *ptr_to_filename)
		{
			const char *const engine_filename = *ptr_to_filename;

			try
			{
				// Keep the engine-owned path as raw bytes. std::filesystem::path(const
				// char*) converts through the active Windows code page and previously
				// terminated the game with ERROR_NO_UNICODE_TRANSLATION (1113).
				const std::string filename(engine_filename);
				const auto modded_xml_filename_lowered = get_normalized_original_filename(filename);
				const auto xml_context                 = get_xml_context(filename);
				const auto context_it                  = g_xml_context_to_modded_xml_filenames.find(xml_context);

				if (context_it != g_xml_context_to_modded_xml_filenames.end()
				    && context_it->second.contains(modded_xml_filename_lowered))
				{
					LOG(INFO) << "[XML Merger] Skipping vanilla parsing of " << filename
					          << " since we already merged it. xml context: " << xml_context;
					return true;
				}
			}
			catch (const std::exception &e)
			{
				LOG(ERROR) << "[XML Merger] Failed to inspect engine path at "
				           << static_cast<const void *>(engine_filename) << ": " << e.what()
				           << ". Falling back to vanilla XML parsing.";
			}
			catch (...)
			{
				LOG(ERROR) << "[XML Merger] Failed to inspect engine path at "
				           << static_cast<const void *>(engine_filename)
				           << " due to an unknown exception. Falling back to vanilla XML parsing.";
			}
		}

		return big::g_hooking->get_original<hook_XmlParserReadOnly_Read_caller>()(a1, ptr_to_filename, a3);
	}

	static __int64 g_CXConsole = 0;
	static uintptr_t *g_CXConsole_pointer = nullptr;
	static void **g_expected_CXConsole_vtable = nullptr;

	static cry_cvar *find_engine_cvar(std::string_view name)
	{
		const std::string owned(name);
		if (const auto found = g_cvars.find(owned);
		    found != g_cvars.end() && found->second)
		{
			return found->second;
		}
		if (!g_CXConsole || owned.empty())
		{
			return nullptr;
		}

		using get_cvar = cry_cvar *(__fastcall *)(__int64, const char *);
		auto **vtable = *reinterpret_cast<void ***>(g_CXConsole);
		if (!vtable || !vtable[23])
		{
			return nullptr;
		}
		auto *cvar =
		    reinterpret_cast<get_cvar>(vtable[23])(g_CXConsole, owned.c_str());
		if (cvar)
		{
			g_cvars.insert_or_assign(owned, cvar);
		}
		return cvar;
	}

	static bool attach_existing_engine_console()
	{
		if (g_CXConsole)
		{
			return true;
		}
		if (!g_CXConsole_pointer || !g_expected_CXConsole_vtable)
		{
			LOG(ERROR) << "Late CXConsole attach is missing signature-gated targets.";
			return false;
		}

		const auto console = static_cast<__int64>(*g_CXConsole_pointer);
		if (!console)
		{
			LOG(WARNING) << "Late CXConsole attach found a null engine console pointer.";
			return false;
		}
		auto **vtable = *reinterpret_cast<void ***>(console);
		if (vtable != g_expected_CXConsole_vtable)
		{
			LOG(ERROR) << "Late CXConsole attach rejected an unexpected engine console vtable.";
			return false;
		}

		g_CXConsole = console;
		LOG(INFO) << "Attached to the existing engine console after retail initialization.";
		return true;
	}

	bool engine_console_available()
	{
		return g_CXConsole != 0;
	}

	bool engine_console_has_command(std::string_view name)
	{
		if (g_console_command_name_to_help_text.contains(std::string(name)))
		{
			return true;
		}

		// A late attach cannot replay commands registered before our hooks were
		// enabled. These two built-ins are verified for the signature-gated retail
		// image and are the only vanilla commands used by the sandbox wrapper.
		return g_CXConsole && (name == "map" || name == "unload");
	}

	bool engine_cvar_available(std::string_view name)
	{
		return find_engine_cvar(name) != nullptr;
	}

	std::optional<int> engine_cvar_int(std::string_view name)
	{
		auto *cvar = find_engine_cvar(name);
		if (!cvar)
		{
			return std::nullopt;
		}
		return cvar->GetIVal();
	}

	std::optional<std::string> engine_cvar_string(std::string_view name)
	{
		auto *cvar = find_engine_cvar(name);
		if (!cvar)
		{
			return std::nullopt;
		}
		const auto *value = cvar->GetString();
		return value ? std::optional(std::string(value)) : std::nullopt;
	}

	bool engine_cvar_set_int(std::string_view name, int value)
	{
		auto *cvar = find_engine_cvar(name);
		if (!cvar)
		{
			return false;
		}
		const auto previous = cvar->GetIVal();
		if (previous == value)
		{
			return true;
		}

		// The supported retail image calls ICVar::Set(int) through VTable slot
		// 7 (+0x38). Calling the overloaded declaration directly is not safe
		// across the retail MSVC ABI because the local overload order is not
		// part of the signature audit.
		using set_int = void(__fastcall *)(cry_cvar *, int);
		auto **vtable = *reinterpret_cast<void ***>(cvar);
		if (!vtable || !vtable[7])
		{
			return false;
		}
		reinterpret_cast<set_int>(vtable[7])(cvar, value);
		const auto actual = cvar->GetIVal();
		if (actual != value)
		{
			LOGF(
			    ERROR,
			    "Retail CVar '{}' rejected integer change {} -> {}; actual value is {}.",
			    name,
			    previous,
			    value,
			    actual);
			return false;
		}
		return true;
	}

	bool engine_console_execute(
	    std::string_view command,
	    bool defer_execution)
	{
		if (!g_CXConsole || command.empty())
		{
			return false;
		}
		using execute_string = void(__fastcall *)(
		    __int64,
		    const char *,
		    bool,
		    bool);
		auto **vtable = *reinterpret_cast<void ***>(g_CXConsole);
		if (!vtable || !vtable[35])
		{
			return false;
		}
		const std::string owned(command);
		reinterpret_cast<execute_string>(vtable[35])(
		    g_CXConsole,
		    owned.c_str(),
		    false,
		    defer_execution);
		return true;
	}

	static engine_console_command_queue g_engine_console_commands;

	engine_console_submit_status queue_engine_console_command(
	    std::string command)
	{
		return g_engine_console_commands.submit(
		    std::move(command),
		    engine_console_available());
	}

	static void execute_queued_engine_console_commands()
	{
		for (const auto &command : g_engine_console_commands.drain(8))
		{
			if (!engine_console_execute(command, true))
			{
				LOGF(
				    ERROR,
				    "Queued local engine-console command could not be executed: '{}'.",
				    command);
			}
		}
	}

	std::string to_string_us(float value)
	{
		std::ostringstream oss;
		oss.imbue(std::locale("C")); // Ensures decimal point is '.'
		oss << value;
		return oss.str();
	}

	static __int64 hook_CXConsole_RegisterVar(__int64 a1, cry_cvar *pCvar, __int64 pChangeFunc)
	{
		// https://github.com/ValtoGameEngines/CryEngine/blob/d9d2c9f000836f0676e65a90bed40dcc3b1451eb/Code/CryEngine/CryCommon/CrySystem/IConsole.h#L612
		const char *cvar_name      = pCvar->GetName();
		const char *cvar_help_text = pCvar->GetHelpText();
		const auto cvar_type       = pCvar->GetType();
		std::string default_value  = "";
		constexpr int CVAR_FLOAT   = 2;
		constexpr int CVAR_INT     = 1;
		constexpr int CVAR_STRING  = 3;
		if (cvar_type == CVAR_FLOAT || cvar_type == CVAR_INT)
		{
			default_value = to_string_us(pCvar->GetFVal());
		}
		else if (cvar_type == CVAR_STRING)
		{
			default_value = pCvar->GetString();
		}

		g_cvar_name_to_cvar_data[cvar_name] = {cvar_help_text, default_value};
		g_cvars[cvar_name] = pCvar;

		const auto res = big::g_hooking->get_original<hook_CXConsole_RegisterVar>()(a1, pCvar, pChangeFunc);

		return res;
	}

	static __int64 hook_CXConsole_AddCommandScript(__int64 a1, const char *sName, __int64 pFunc, int nFlags, const char *sHelp)
	{
		if (sName)
		{
			g_console_command_name_to_help_text[sName] = sHelp ? sHelp : "";
		}

		const auto res = big::g_hooking->get_original<hook_CXConsole_AddCommandScript>()(a1, sName, pFunc, nFlags, sHelp);
		return res;
	}

	static __int64 hook_CXConsole_AddCommandCommand(__int64 a1, const char *sName, __int64 pFunc, int nFlags, const char *sHelp)
	{
		if (sName)
		{
			g_console_command_name_to_help_text[sName] = sHelp ? sHelp : "";
		}

		const auto res = big::g_hooking->get_original<hook_CXConsole_AddCommandCommand>()(a1, sName, pFunc, nFlags, sHelp);
		return res;
	}

	static __int64 hook_CXConsole_Ctor(__int64 a1)
	{
		const auto res = big::g_hooking->get_original<hook_CXConsole_Ctor>()(a1);

		g_CXConsole = a1;

		return res;
	}

	static __int64 hook_CEntitySystem_CEntitySystem(CEntitySystem *this_, __int64 ISystem_pSystem)
	{
		const auto res = big::g_hooking->get_original<hook_CEntitySystem_CEntitySystem>()(this_, ISystem_pSystem);

		g_CEntitySystem = this_;

		return res;
	}

	static __int64 hook_CEntity_dctor(CEntity *centity_inst, char a2)
	{
		if (g_player_entity == centity_inst)
		{
			g_player_entity = nullptr;
			LOG(INFO) << "Player entity released";
		}
		const auto res = big::g_hooking->get_original<hook_CEntity_dctor>()(centity_inst, a2);

		const auto it = std::find(g_entities.begin(), g_entities.end(), centity_inst);
		if (it != g_entities.end())
		{
			*it = g_entities.back();
			g_entities.pop_back();
		}
		g_entity_infos.erase(centity_inst);

		return res;
	}

	static __int64 hook_CEntity_ctor(CEntity *a1, __int64 a2)
	{
		const auto res = big::g_hooking->get_original<hook_CEntity_ctor>()(a1, a2);

		g_entities.push_back(a1);
		CEntitySystem_DumpEntity(g_CEntitySystem, a1);

		if (strcmp(a1->GetName(), "Dude") == 0)
		{
			g_player_entity = a1;
			LOG(INFO) << "Player entity found";
		}

		return res;
	}

	static __int64 hook_CStatObj_dctor(uintptr_t this_, char a2)
	{
		const auto res = big::g_hooking->get_original<hook_CStatObj_dctor>()(this_, a2);

		g_cstatobjs.erase(this_);

		return res;
	}

	static void *hook_CStatObj_ctor(uintptr_t this_)
	{
		const auto res = big::g_hooking->get_original<hook_CStatObj_ctor>()(this_);

		g_cstatobjs.insert(this_);

		return res;
	}

	static __int64 hook_CGeomCacheRenderNode_dctor(IRenderNode *this_, char a2)
	{
		const auto res = big::g_hooking->get_original<hook_CGeomCacheRenderNode_dctor>()(this_, a2);

		g_CGeomCacheRenderNodes.erase(this_);

		return res;
	}

	static void *hook_CGeomCacheRenderNode_ctor(IRenderNode *this_)
	{
		const auto res = big::g_hooking->get_original<hook_CGeomCacheRenderNode_ctor>()(this_);

		g_CGeomCacheRenderNodes.insert(this_);

		return res;
	}

	static __int64 hook_CVegetation_dctor(IRenderNode *this_, char a2)
	{
		const auto res = big::g_hooking->get_original<hook_CVegetation_dctor>()(this_, a2);

		g_CVegetations.erase(this_);

		return res;
	}

	static void *hook_CVegetation_ctor(IRenderNode *this_)
	{
		const auto res = big::g_hooking->get_original<hook_CVegetation_ctor>()(this_);

		g_CVegetations.insert(this_);

		return res;
	}

	static __int64 hook_CMergedMeshRenderNode_dctor(IRenderNode *this_, char a2)
	{
		const auto res = big::g_hooking->get_original<hook_CMergedMeshRenderNode_dctor>()(this_, a2);

		const auto it = std::find(g_CMergedMeshRenderNodes.begin(), g_CMergedMeshRenderNodes.end(), this_);
		if (it != g_CMergedMeshRenderNodes.end())
		{
			*it = g_CMergedMeshRenderNodes.back();
			g_CMergedMeshRenderNodes.pop_back();
		}

		return res;
	}

	static void *hook_CMergedMeshRenderNode_ctor(IRenderNode *this_)
	{
		const auto res = big::g_hooking->get_original<hook_CMergedMeshRenderNode_ctor>()(this_);

		g_CMergedMeshRenderNodes.push_back(this_);

		return res;
	}

	static __int64 hook_CBrush_dctor(CBrush *inst, char a2)
	{
		const auto res = big::g_hooking->get_original<hook_CBrush_dctor>()(inst, a2);

		const auto it = std::find(g_cbrushes.begin(), g_cbrushes.end(), inst);
		if (it != g_cbrushes.end())
		{
			*it = g_cbrushes.back();
			g_cbrushes.pop_back();
		}

		return res;
	}

	static ankerl::unordered_dense::set<void *> g_CBrush_callers;

	static big::stack_trace trace;

	static int log_stack_trace_filter(EXCEPTION_POINTERS *ep)
	{
		trace.new_stack_trace(ep);

		LOG(ERROR) << trace;
		Logger::FlushQueue();

		return EXCEPTION_EXECUTE_HANDLER;
	}

	static void log_stack_trace()
	{
		__try
		{
			// On purpose to print a stack trace.
			*reinterpret_cast<int *>(0xDE'AD) = 0;
		}
		__except (log_stack_trace_filter(GetExceptionInformation()))
		{
		}
	}

	__int64 __fastcall hook_CMovableBrush_ctor(uintptr_t a1)
	{
		const auto res = big::g_hooking->get_original<hook_CMovableBrush_ctor>()(a1);

		return res;
	}

	PBYTE hook_vtable_func(uintptr_t **ppVTable, PBYTE pHook, SIZE_T iIndex)
	{
		DWORD dwOld = 0;
		VirtualProtect((void *)((*ppVTable) + iIndex), sizeof(PDWORD64), PAGE_EXECUTE_READWRITE, &dwOld);

		PBYTE pOrig         = ((PBYTE)(*ppVTable)[iIndex]);
		(*ppVTable)[iIndex] = (DWORD64)pHook;
		VirtualProtect((void *)((*ppVTable) + iIndex), sizeof(PDWORD64), dwOld, &dwOld);

		return pOrig;
	}

	static EERType CMovableBrush_GetRenderNodeType()
	{
		//return eERType_MovableBrush;
		return eERType_Brush;
	}

	static __int64 hook_CBrush_ctor(CBrush *a1, __int64 a2)
	{
		//const auto ret_address = _ReturnAddress();

		//if (!g_CBrush_callers.contains(ret_address))
		//{
		//g_CBrush_callers.insert(ret_address);

		//#define HEX_TO_UPPER_OFFSETWH(value) \
	//"0x" << std::hex << std::uppercase << ((DWORD64)value - (DWORD64)GetModuleHandleA("WHGame.dll")) << std::dec << std::nouppercase
		//LOG(ERROR) << "CBrush caller: " << HEX_TO_UPPER_OFFSETWH(ret_address);

		//log_stack_trace();
		//}

		//const auto res = big::g_hooking->get_original<hook_CMovableBrush_ctor>()((uintptr_t)a1);
		const auto res = big::g_hooking->get_original<hook_CBrush_ctor>()(a1, a2);

		//static auto hook_vtable_once = true;
		//if (hook_vtable_once)
		//{
		//hook_vtable_once = false;
		//hook_vtable_func((*reinterpret_cast<uintptr_t ***>(a1)), (PBYTE)CMovableBrush_GetRenderNodeType, 7);
		//}

		g_cbrushes.push_back(a1);

		//LOG(INFO) << HEX_TO_UPPER(a1);

		return res;
	}

	//__int64 hook_CPlayerStateMovement_Ledge_callback(__int64 a1, __int64 a2, __int64 a3, int32_t *a4)
	//{
	//const auto res = big::g_hooking->get_original<hook_CPlayerStateMovement_Ledge_callback>()(a1, a2, a3, a4);

	//LOG(INFO) << "CPlayerStateMovement_Ledge_callback callback: " << a1 << " - " << a2 << " - " << a3 << " - " << a4;

	//LOG(INFO) << "a1: " << typeid(*reinterpret_cast<vtable_helper *>(a1)).name(); // C_PlayerStateMovement

	// a2 is a 40 bytes struct, can be anything, doesn't matter, just allocate it on stack.
	// a4 is the int enum new value of the state.

	//LOG(INFO) << "a3: " << typeid(*reinterpret_cast<vtable_helper *>(a3)).name(); // C_Player

	//return res;
	//}

	__int64 hook_C_PlayerStateMovement_ctor(__int64 a1)
	{
		g_C_PlayerStateMovement = a1;

		const auto res = big::g_hooking->get_original<hook_C_PlayerStateMovement_ctor>()(a1);

		return res;
	}

	__int64 hook_C_Player_ctor(__int64 a1)
	{
		g_C_Player = a1;

		const auto res = big::g_hooking->get_original<hook_C_Player_ctor>()(a1);

		return res;
	}

	std::recursive_mutex g_rendernodes_mutex;
	static kcd2_address g_C3DEngine_UnRegisterEntityImpl_ptr;

	inline void hook_C3DEngine_UnRegisterEntityImpl(uintptr_t this_, IRenderNode *node)
	{
		big::g_hooking->get_original<hook_C3DEngine_UnRegisterEntityImpl>()(this_, node);

		std::scoped_lock l(g_rendernodes_mutex);
		g_rendernodes.erase(node);
	}

	bool is_render_node_valid(IRenderNode *node)
	{
		__try
		{
			node->GetPhysics();
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}

		return false;
	}

	inline void hook_C3DEngine_RegisterEntity(uintptr_t this_, IRenderNode *node)
	{
		big::g_hooking->get_original<hook_C3DEngine_RegisterEntity>()(this_, node);

		std::scoped_lock l(g_rendernodes_mutex);

		if (is_render_node_valid(node))
		{
			g_rendernodes.insert(node);
		}
	}

	__int64 hook_C3DEngine_ctor(__int64 a1, __int64 a2)
	{
		g_C3DEngine = a1;

		const auto res = big::g_hooking->get_original<hook_C3DEngine_ctor>()(a1, a2);

		return res;
	}

	/*
		sx (float) - The x-coordinate in screen space.
		sy (float) - The y-coordinate in screen space.
		sz (float) - The depth value (typically between 0.0 and 1.0 in normalized device coordinates).
		px (float)* - Pointer to store the resulting world x-coordinate.
		py (float)* - Pointer to store the resulting world y-coordinate.
		pz (float)* - Pointer to store the resulting world z-coordinate.
	*/
	static __int64 __fastcall hook_CD3D9Renderer_UnProjectFromScreen(__int64 a1, float sx, float sy, float sz, float *px, float *py, float *pz)
	{
		g_CD3D9Renderer = a1;

		const auto res = big::g_hooking->get_original<hook_CD3D9Renderer_UnProjectFromScreen>()(a1, sx, sy, sz, px, py, pz);

		return res;
	}

	static __int64 hook_wh_db_table_patch_find_line(__int64 table_metadata, char *table_vanilla_data, unsigned int table_vanilla_line_index, char *table_mod_data, unsigned int table_mod_line_index)
	{
		const auto res = big::g_hooking->get_original<hook_wh_db_table_patch_find_line>()(table_metadata, table_vanilla_data, table_vanilla_line_index, table_mod_data, table_mod_line_index);

		const auto v5                    = *(uint64_t *)(table_metadata + 64);
		const auto line_size             = *(uint32_t *)(v5 + 16);
		char *table_vanilla_data_indexed = &table_vanilla_data[table_mod_line_index * line_size];

		char *table_vanilla_data_key = *(char **)table_vanilla_data_indexed;
		char *table_mod_data_key_ptr = *(char **)table_mod_data;

		std::string table_mod_data_key = safe_string_convert(table_mod_data_key_ptr);

		std::vector<uint8_t> patched_data(line_size);
		std::memcpy(patched_data.data(), table_mod_data, line_size);

		if (res == -1)
		{
			g_table_name_to_added_line_to_info[g_table_current_table_name][table_mod_data_key].push_back({g_table_current_mod_name, patched_data});
		}
		else
		{
			if (!g_table_name_to_modified_line_to_original_data[g_table_current_table_name].contains(table_mod_data_key))
			{
				std::vector<uint8_t> orig_data(line_size);
				std::memcpy(orig_data.data(), table_vanilla_data_indexed, line_size);
				g_table_name_to_modified_line_to_original_data[g_table_current_table_name][table_mod_data_key] = orig_data;
			}
			g_table_name_to_modified_line_to_info[g_table_current_table_name][table_mod_data_key].push_back({g_table_current_mod_name, patched_data});
		}

		return res;
	}

	// Skip the "__" or return original string if not found
	static const char *get_string_after_double_underscore(const char *str)
	{
		const char *found = strstr(str, "__");
		return found ? found + 2 : str;
	}

	static char __fastcall hook_wh_db_table_patched(__int64 table_metadata, __int64 mod_metadata, int *line_added_count, int *line_modified_count)
	{
		g_table_current_table_name = *(const char **)(table_metadata + 40);

		const auto mod_patch_table_name = *(const char **)(mod_metadata + 40);
		g_table_current_mod_name        = get_string_after_double_underscore(mod_patch_table_name);

		LOG(INFO) << "Table PTF Name: " << g_table_current_table_name << " - " << g_table_current_mod_name;

		const auto is_table_patched = big::g_hooking->get_original<hook_wh_db_table_patched>()(table_metadata, mod_metadata, line_added_count, line_modified_count);
		if (is_table_patched)
		{
		}

		return is_table_patched;
	}

	static bool hook_CCryFile_Open(__int64 this_, const char *filename, const char *mode, unsigned int nOpenFlags)
	{
		if (strcmp(mode, "rb") != 0)
		{
			return big::g_hooking->get_original<hook_CCryFile_Open>()(this_, filename, mode, nOpenFlags);
		}

		if (!g_lua_manager)
		{
			return big::g_hooking->get_original<hook_CCryFile_Open>()(this_, filename, mode, nOpenFlags);
		}

		std::scoped_lock guard(g_lua_manager->m_module_lock);

		for (const auto &mod_ : g_lua_manager->m_modules)
		{
			auto mod      = (lua_module_ext *)mod_.get();
			const auto it = mod->m_data_ext.m_cryfile_open_replace_map.find(filename);
			if (it != mod->m_data_ext.m_cryfile_open_replace_map.end())
			{
				return big::g_hooking->get_original<hook_CCryFile_Open>()(this_, it->second.c_str(), mode, nOpenFlags);
			}
		}

		return big::g_hooking->get_original<hook_CCryFile_Open>()(this_, filename, mode, nOpenFlags);
	}

	void apply_xml_patches(std::string &originalFileContent, const std::vector<std::string> &patchFileContents, bool is_inventory_preset)
	{
		pugi::xml_document originalDoc;
		pugi::xml_parse_result result = originalDoc.load_buffer(originalFileContent.c_str(), originalFileContent.size(), (pugi::parse_default | pugi::parse_comments));
		if (!result)
		{
			LOG(ERROR) << "Failed to parse original file: " << result.description();
			return;
		}

		for (const std::string &patchContent : patchFileContents)
		{
			pugi::xml_document patchDoc;
			pugi::xml_parse_result patchResult = patchDoc.load_buffer(patchContent.c_str(), patchContent.size(), (pugi::parse_default | pugi::parse_comments));
			if (!patchResult)
			{
				LOG(ERROR) << "Failed to parse patch file: " << patchResult.description();
				continue;
			}

			auto get_node_identifier = [](pugi::xml_node &node, bool is_inventory_preset)
			{
				std::string identifier;
				if (is_inventory_preset)
				{
					identifier += node.attribute("Name").value();
					identifier += node.attribute("Ref").value();
				}
				else // objects_mission0.xml
				{
					identifier += node.attribute("EntityId").value();
					identifier += node.attribute("TargetId").value();
					identifier += node.attribute("TargetGuid").value();
				}

				return identifier;
			};

			std::function<void(pugi::xml_node, pugi::xml_node)> merge_nodes;
			merge_nodes = [&](pugi::xml_node original_node, pugi::xml_node patch_node)
			{
				for (pugi::xml_node patch_child : patch_node.children())
				{
					const char *name = patch_child.name();

					std::string patch_identifier = get_node_identifier(patch_child, is_inventory_preset);

					pugi::xml_node original_matching_node;
					for (pugi::xml_node base_child : original_node.children(name))
					{
						std::string original_identifier = get_node_identifier(base_child, is_inventory_preset);

						if (patch_identifier == original_identifier)
						{
							original_matching_node = base_child;
							break;
						}
					}

					if (original_matching_node)
					{
						for (pugi::xml_attribute attr : patch_child.attributes())
						{
							const auto attr_name = attr.name();
							auto original_attr   = original_matching_node.attribute(attr_name);
							if (!original_attr)
							{
								original_attr = original_matching_node.append_attribute(attr_name);
							}
							original_attr.set_value(attr.value());
						}
						merge_nodes(original_matching_node, patch_child);
					}
					else
					{
						original_node.append_copy(patch_child);
					}
				}
			};

			merge_nodes(originalDoc, patchDoc);
		}

		std::ostringstream outputStream;
		originalDoc.save(outputStream);
		originalFileContent = outputStream.str();

		//std::ofstream file("C:/Users/Quentin/Desktop/objects_mission0_PATCHED.xml", std::ios::binary);
		//if (file)
		//{
		//file.write(originalFileContent.data(), originalFileContent.size());
		//}
	}

	void read_xmls_from_zip_stream(struct zip_t *zip, const std::string &xml_context = "global")
	{
		int n = zip_entries_total(zip);
		for (int i = 0; i < n; ++i)
		{
			zip_entry_openbyindex(zip, i);
			const std::string zip_entry_name_ = zip_entry_name(zip);
			int isdir                         = zip_entry_isdir(zip);
			unsigned long long size           = zip_entry_size(zip);

			if (isdir || size == 0)
			{
				zip_entry_close(zip);
				continue;
			}

			std::vector<unsigned char> file_content(size);
			zip_entry_noallocread(zip, file_content.data(), size);
			zip_entry_close(zip);

			std::filesystem::path entry_path = zip_entry_name_;
			std::string entry_lower          = big::string::to_lower(entry_path.filename().string());

			if (entry_lower.ends_with(".xml"))
			{
				if (entry_lower.contains("inventorypreset") || entry_lower.contains("objects_mission0"))
				{
					g_xml_context_to_modded_xml_filenames[xml_context].insert(entry_lower);
					const auto original_filename = get_original_filename_from_mod_filename(zip_entry_name_);
					std::string modification(file_content.begin(), file_content.end());
					g_xml_context_to_xml_filename_to_modifications[xml_context][original_filename].emplace_back(std::move(modification));

					LOG(INFO) << "[XML Merger] Adding modded xml file " << entry_lower << " to " << original_filename << " patch list (count: "
					          << g_xml_context_to_xml_filename_to_modifications[xml_context][original_filename].size() << ") (xml context: " << xml_context << ")";
				}
			}
			else if (entry_lower.ends_with(".pak"))
			{
				struct zip_t *inner_zip = zip_stream_open((const char *)file_content.data(), size, 0, 'r');
				if (inner_zip)
				{
					const auto xml_context_ = get_xml_context(zip_entry_name_);
					read_xmls_from_zip_stream(inner_zip, xml_context_);
					zip_close(inner_zip);
				}
			}
		}
	}

	void read_xmls_from_zip_file_path(const std::string &zipFilePath)
	{
		struct zip_t *zip = zip_open(zipFilePath.c_str(), 0, 'r');
		if (!zip)
		{
			return;
		}

		read_xmls_from_zip_stream(zip);

		zip_close(zip);
	}

	void CEntitySystem_DumpEntity(CEntitySystem *entity_system, CEntity *entity)
	{
		EntityInfo info;

		info.name       = entity->GetName();
		info.name_lower = big::string::to_lower(info.name);

		auto entity_class     = entity->GetClass();
		info.class_name       = entity_class->GetName();
		info.class_name_lower = big::string::to_lower(info.class_name);

		Archetype *archetype = entity->m_archetype;
		if (archetype)
		{
			//LOG(INFO) << HEX_TO_UPPER(archetype);
			//Logger::FlushQueue();
			//MessageBoxA(0, "", "", 0);
			info.archetype_name       = archetype->m_name;
			info.archetype_name_lower = big::string::to_lower(info.archetype_name);
		}
		else
		{
			info.archetype_name       = "None";
			info.archetype_name_lower = big::string::to_lower(info.archetype_name);
		}

		//entity->GetPos(info.position);
		//info.is_active = entity->IsActive();
		//info.is_hidden = entity->IsHidden();

		info.id        = entity->GetId();
		info.id_string = std::to_string(info.id);
		info.id_mask   = info.id & 0x3'FF'FF;
		info.id_salt   = info.id >> 18;

		__int64 entity_layer_data = entity_system->GetEntityLayerData(entity);
		if (entity_layer_data)
		{
			info.layer_id         = *(unsigned __int16 *)(entity_layer_data + 40);
			info.layer_name       = *(const char **)(entity_layer_data + 16);
			info.layer_name_lower = big::string::to_lower(info.layer_name);
		}

		auto guid_data   = entity->GetGuid();
		auto format_guid = [](__int64 guid_data) -> std::string
		{
			char result[64];
			snprintf(result, sizeof(result), "%.8x-%.4x-%.4x", *(uint32_t *)guid_data, *(uint16_t *)(guid_data + 4), *(uint16_t *)(guid_data + 6));
			return std::string(result);
		};
		if (guid_data)
		{
			info.guid = format_guid((__int64)&guid_data);
		}

		g_entity_infos.insert({entity, info});
	}

	void CEntitySystem_DumpEntities(CEntitySystem *entity_system)
	{
		g_entity_infos.clear();
		g_entities_filtered.clear();

		auto it = entity_system->GetEntityIterator();
		if (it)
		{
			(*(void (**)(__int64))(*(__int64 *)it + 8LL))(it);
		}

		const auto EntityIterator_MoveFirst_func = (*(void (**)(__int64))(*(__int64 *)it + 48LL));
		EntityIterator_MoveFirst_func(it);

		const auto EntityIterator_Next_func = (*(CEntity * (**)(__int64))(*(__int64 *)it + 32LL));
		while (auto entity = EntityIterator_Next_func(it))
		{
			CEntitySystem_DumpEntity(entity_system, entity);
		}
	}

	uintptr_t isystem_get_camera()
	{
		// vtable offset retrieved by looking at the lua exposed function "GetViewCameraPos"

		const auto ISystem_GetCamera_func = (*reinterpret_cast<void ***>(g_ISystem))[133];
		return ((uintptr_t (*)(uintptr_t))ISystem_GetCamera_func)(g_ISystem);
	}

	std::pair<Vec3, Vec3> get_camera_position_and_direction()
	{
		// offsets retrieved by looking at the lua exposed function "GetViewCameraPos"

		float *camera_matrix = (float *)isystem_get_camera();
		Vec3 camera_position;
		camera_position.x = camera_matrix[3];
		camera_position.y = camera_matrix[7];
		camera_position.z = camera_matrix[11];

		Vec3 camera_direction;
		camera_direction.x = camera_matrix[1];
		camera_direction.y = camera_matrix[5];
		camera_direction.z = camera_matrix[9];

		return {camera_position, camera_direction};
	}

	std::pair<float *, float *> get_camera_view_and_projection_matrix()
	{
		// offsets retrieved by looking at CD3D9Renderer_UnProjectFromScreen

		uintptr_t ccamera = isystem_get_camera();

		const auto view_matrix44 = (float *)(ccamera + 548);
		const auto proj_matrix44 = (float *)(ccamera + 548 + (4 * 4 * sizeof(float)));

		return {view_matrix44, proj_matrix44};
	}

	void target_entity_on_crosshair()
	{
		if (!g_player_entity)
		{
			return;
		}

		auto camera_pos_and_dir = get_camera_position_and_direction();

		Vec3 ray_dir_range = camera_pos_and_dir.second * 100000.0f;

		void *pSkipEnts[1];
		pSkipEnts[0] = g_player_entity->GetPhysics();

		ray_hit_t ray_hit;
		if (RayWorldIntersection(camera_pos_and_dir.first, ray_dir_range, ent_all, rwi_stop_at_pierceable | rwi_colltype_any, &ray_hit, 1, pSkipEnts, 1))
		{
			const auto hit_entity = (CEntity *)ray_hit.pCollider->GetForeignData(PHYS_FOREIGN_ID_ENTITY);
			if (hit_entity)
			{
				LOG(INFO) << "[Raycast] Entity hit: " << hit_entity->GetName();

				size_t k = 0;
				for (const auto &ent : g_entities)
				{
					if (ent == hit_entity)
					{
						g_selected_index_entity_detail_inspector = k;
						return;
					}

					k++;
				}
			}
		}

		g_selected_index_entity_detail_inspector = -1;
	}

	EERType safe_render_node_type(IRenderNode *node)
	{
		EERType render_node_type = eERType_NotRenderNode;
		__try
		{
			render_node_type = node->GetRenderNodeType();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return eERType_NotRenderNode;
		}

		return render_node_type;
	}

	std::mutex g_cphysicalentity_mutex;

	static bool safe_unregister_rendernode(IRenderNode *render_node)
	{
		__try
		{
			big::g_hooking->get_original<hook_C3DEngine_UnRegisterEntityImpl>()(g_C3DEngine, render_node);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void unregister_all_rendernode()
	{
		for (auto render_node : g_rendernodes)
		{
			if (!safe_unregister_rendernode(render_node))
			{
				continue;
			}
		}
	}

	void offset_every_object_around()
	{
		if (!g_player_entity)
		{
			return;
		}

		LOG(INFO) << "offset_every_object_around";

		auto camera_pos_and_dir = get_camera_position_and_direction();

		// Extend the max range distance
		Vec3 ray_dir_range = camera_pos_and_dir.second * 100000.0f;

		void *pSkipEnts[1];
		pSkipEnts[0] = g_player_entity->GetPhysics();

		ray_hit_t ray_hit;
		if (RayWorldIntersection(camera_pos_and_dir.first, ray_dir_range, ent_all, rwi_stop_at_pierceable | rwi_colltype_any, &ray_hit, 1, pSkipEnts, 1))
		{
			CEntity *hit_entity{};
			if (ray_hit.pCollider)
			{
				hit_entity = (CEntity *)ray_hit.pCollider->GetForeignData(PHYS_FOREIGN_ID_ENTITY);
			}
			if (hit_entity)
			{
				LOG(INFO) << "Found entity.";
			}
			else
			{
				if (!g_cbrushes.empty())
				{
					LOG(INFO) << "Trying to find cbrush.";

					size_t best_cbrush_index = 0;
					float min_distance       = std::numeric_limits<float>::max();
					Vec3 hit_position        = ray_hit.pt;

					for (size_t i = 0; i < g_cbrushes.size(); ++i)
					{
						const auto cbrush_pos = g_cbrushes[i]->GetPos();

						float current_distance = (hit_position - cbrush_pos).Length();

						if (current_distance < min_distance)
						{
							min_distance      = current_distance;
							best_cbrush_index = i;
						}
					}

					g_selected_cbrush_detail_inspector = best_cbrush_index;

					auto brush_name = g_cbrushes[best_cbrush_index]->GetName();
					auto brush_pos  = g_cbrushes[best_cbrush_index]->GetPos();
					LOG(INFO) << "Best cbrush id found: " << best_cbrush_index << " Name: " << (brush_name ? brush_name : "No Name")
					          << " Position: " << brush_pos.x << " " << brush_pos.y << " " << brush_pos.z;

					//const Vec3 offset_pos = {0, 0, 500.0f};
					const Vec3 offset_pos = {0, 0, 0};


					//static auto ResetTerrainVertBuffers =

					//ResetTerrainVertBuffers.as_func<void(__int64, void *)>()(g_CTerrain, nullptr);

					// move every cbrush position around the best cbrush found, all cbrush within 100 of distance.
					for (size_t i = 0; i < g_cbrushes.size(); ++i)
					{
						const auto cbrush_pos  = g_cbrushes[i]->GetPos();
						float current_distance = (cbrush_pos - brush_pos).Length();
						if (current_distance < 1000.0f)
						{
							float new_matrix[3 * 4];
							memcpy(new_matrix, g_cbrushes[i]->m_Matrix, sizeof(new_matrix));
							//new_matrix[3]  = new_matrix[3] + 50.0f;
							//new_matrix[7]  = new_matrix[7] + 50.0f;
							//new_matrix[11] = new_matrix[11] + 50.0f;

							//g_cbrushes[i]->SetMatrix(new_matrix);

							//g_cbrushes[i]->m_bNoPhysicalize = false;
							//#define BIT64(x) (1ull << (x))
							//g_cbrushes[i]->m_dwRndFlags &= ~(BIT64(39)); // ERF_NO_3DENGINE_REGISTRATION

							//auto aabb_obj = g_cbrushes[i]->GetBBox();

							//g_cbrushes[i]->OffsetPosition(offset_pos);
							//C3DEngine_UnRegisterEntityDirect(g_cbrushes[i]);

							//g_cbrushes[i]->Dephysicalize();
							//C3DEngine_UnRegisterEntityDirect(g_cbrushes[i]);

							// GetTerrain()->ResetTerrainVertBuffers(&aabb);
							//(*(void(__fastcall **)(__int64, void *))(*(uintptr_t *)g_CTerrain + 152))(g_CTerrain, &aabb_obj);

							//C3DEngine_RegisterEntity(g_cbrushes[i]);
							//g_cbrushes[i]->Physicalize(true);
							//LOG(INFO) << "Moved cbrush id: " << i << " to position: " << new_matrix[3] << " " << new_matrix[7] << " " << new_matrix[11];
						}
					}

					//const Vec3 offset_pos = {50.0f, 50.0f, 50.0f};
					for (size_t i = 0; i < g_CMergedMeshRenderNodes.size(); ++i)
					{
						const auto cbrush_pos  = g_CMergedMeshRenderNodes[i]->GetPos();
						float current_distance = (cbrush_pos - brush_pos).Length();
						if (current_distance < 1000.0f)
						{
							//g_CMergedMeshRenderNodes[i]->OffsetPosition(offset_pos);
							//C3DEngine_UnRegisterEntityDirect(g_CMergedMeshRenderNodes[i]);

							//g_CMergedMeshRenderNodes[i]->Dephysicalize();
							//C3DEngine_UnRegisterEntityDirect(g_CMergedMeshRenderNodes[i]);

							//auto aabb_obj = g_CMergedMeshRenderNodes[i]->GetBBox();

							// GetTerrain()->ResetTerrainVertBuffers(&aabb);
							//(*(void(__fastcall **)(__int64, void *))(*(uintptr_t *)g_CTerrain + 152LL))(g_CTerrain, &aabb_obj);
							//ResetTerrainVertBuffers.as_func<void(__int64, void *)>()(g_CTerrain, &aabb_obj);

							//C3DEngine_RegisterEntity(g_CMergedMeshRenderNodes[i]);
							//g_CMergedMeshRenderNodes[i]->Physicalize(true);
						}
					}

					//std::scoped_lock ll(g_rendernodes_mutex);
					//unregister_all_rendernode();

					std::scoped_lock l(g_cphysicalentity_mutex);
					LOG(ERROR) << "dephys " << g_physicalentities.size();
					//for (auto render_node : g_physicalentities)
					for (auto physent : g_physicalentities)
					{
						if (!physent)
						{
							continue;
						}


						//pe_params_pos pos_params{};
						//if (getparams_safe(physent, pos_params))
						//{
						//float current_distance = (pos_params.pos - brush_pos).Length();
						//if (current_distance < 1000.0f)
						//{
						//pos_params.pos += offset_pos;
						//physent->SetParams(&pos_params);
						//}
						//}

						//EERType render_node_type = safe_render_node_type(render_node);

						//if (render_node_type != eERType_NotRenderNode && render_node_type != eERType_Character)
						//{
						//render_node->Dephysicalize();
						//}

						// clang-format off
						/*if (render_node_type == eERType_Brush ||
							render_node_type == eERType_MergedMeshInstance ||
							render_node_type == eERType_Decal ||
							render_node_type == eERType_Road ||
							render_node_type == eERType_MovableBrush ||
							render_node_type == eERType_MergedMesh ||
							render_node_type == eERType_GeomCache
							)
						{
							render_node->Dephysicalize();
						}*/
						// clang-format on
					}
				}
			}
		}
	}

	void target_entity_on_crosshair_include_cbrush()
	{
		if (!g_player_entity)
		{
			return;
		}

		LOG(INFO) << "target_entity_on_crosshair_include_cbrush";

		auto camera_pos_and_dir = get_camera_position_and_direction();

		// Extend the max range distance
		Vec3 ray_dir_range = camera_pos_and_dir.second * 100000.0f;

		void *pSkipEnts[1];
		pSkipEnts[0] = g_player_entity->GetPhysics();

		ray_hit_t ray_hit;
		if (RayWorldIntersection(camera_pos_and_dir.first, ray_dir_range, ent_all, rwi_stop_at_pierceable | rwi_colltype_any, &ray_hit, 1, pSkipEnts, 1))
		{
			if (ray_hit.pCollider)
			{
				LOG(INFO) << "Found collider " << typeid(*ray_hit.pCollider).name() << " " << HEX_TO_UPPER(ray_hit.pCollider);

				LOG(INFO) << "is ray hit point inside: " << ray_hit.pCollider->IsPointInside(ray_hit.pt);

				const auto collider_cbrush = ray_hit.pCollider->GetForeignData(PHYS_FOREIGN_ID_STATIC);

				size_t i = 0;
				for (const auto &cbrush : g_cbrushes)
				{
					if (cbrush == collider_cbrush)
					{
						g_selected_cbrush_detail_inspector = i;
						LOG(INFO) << "Found cbrush: " << cbrush->GetName() << " " << HEX_TO_UPPER(cbrush);
						break;
					}

					i++;
				}

				if (strstr(typeid(*ray_hit.pCollider).name(), "CPhysicalEntity"))
				{
					if (0)
					{
						const auto pos_offset = Vec3(0, 0, 50);

						pe_params_bbox bboxparam;
						ray_hit.pCollider->GetParams(&bboxparam);
						bboxparam.BBox.min += pos_offset;
						bboxparam.BBox.max += pos_offset;
						ray_hit.pCollider->SetParams(&bboxparam);

						pe_params_part part_params;
						ray_hit.pCollider->GetParams(&part_params);
						part_params.pos += pos_offset;
						ray_hit.pCollider->SetParams(&part_params);

						for (const auto render_node : g_cbrushes)
						{
							if (ray_hit.pCollider->IsPointInside(render_node->GetPos()))
							{
								render_node->OffsetPosition(pos_offset);

								LOG(INFO) << render_node->GetName() << " " << render_node->GetEntityClassName() << " "
								          << typeid(*render_node).name();
							}
						}
					}
				}

				for (size_t i = 0; i < 200; i++)
				{
					if (ray_hit.pCollider->GetForeignData(i))
					{
						LOG(INFO) << "Collider type: " << i;
					}
				}
			}
			else
			{
				LOG(INFO) << "No collider found.";
			}
		}
		else
		{
			LOG(INFO) << "Didn't hit shit.";
		}
	}

	static void CHardwareMouse_GetHardwareMouseClientPosition(float *pfX, float *pfY)
	{
		POINT pointCursor;
		GetCursorPos(&pointCursor);
		ScreenToClient(g_renderer->m_window_handle, &pointCursor);
		*pfX = (float)pointCursor.x;
		*pfY = (float)pointCursor.y;
	}

	void target_entity_on_screen_cursor()
	{
		if (!g_player_entity)
		{
			return;
		}

		if (!g_gui)
		{
			return;
		}

		if (!g_gui->is_open())
		{
			return;
		}

		if (ImGui::IsAnyItemHovered())
		{
			return;
		}

		float mouseX, mouseY;
		CHardwareMouse_GetHardwareMouseClientPosition(&mouseX, &mouseY);

		ImVec2 displaySize = ImGui::GetIO().DisplaySize;

		// Invert mouse Y
		mouseY = displaySize.y - mouseY;

		Vec3 vPos0(0, 0, 0);
		g_CD3D9Renderer_UnProjectFromScreen((void *)g_CD3D9Renderer, mouseX, mouseY, 0, &vPos0.x, &vPos0.y, &vPos0.z);

		Vec3 vPos1(0, 0, 0);
		g_CD3D9Renderer_UnProjectFromScreen((void *)g_CD3D9Renderer, mouseX, mouseY, 1, &vPos1.x, &vPos1.y, &vPos1.z);

		Vec3 vDir = (vPos1 - vPos0).Normalized() * 100000.0f;

		void *pSkipEnts[1];
		pSkipEnts[0] = g_player_entity->GetPhysics();

		ray_hit_t hit;
		if (RayWorldIntersection(vPos0, vDir, ent_all, rwi_stop_at_pierceable | rwi_colltype_any, &hit, 1, pSkipEnts, 1))
		{
			const auto hit_entity = (CEntity *)hit.pCollider->GetForeignData(PHYS_FOREIGN_ID_ENTITY);
			if (hit_entity)
			{
				LOG(INFO) << "[Raycast] Entity hit: " << hit_entity->GetName();

				size_t k = 0;
				for (const auto &ent : g_entities)
				{
					if (ent == hit_entity)
					{
						g_selected_index_entity_detail_inspector = k;
						return;
					}

					k++;
				}
			}
		}

		g_selected_index_entity_detail_inspector = -1;
	}

	__int64 __fastcall hook_REGISTER_CVAR(const char *a1, int *a2, int a3, int a4, __int64 a5, __int64 a6)
	{
		// This doesn't seem to do anything
		// Context: CBrush / Existing Level Modifications
		// Was trying to see if disabling that would fix some CBrush not having actual collision thought it was directly baked into the terrain data
		// Which is probably true in some cases,
		// but it seems CryEngine has another optimisation and it was related to unlinked
		// (understand not linked to a CBrush directly) CPhysicalEntity.
		//if (strcmp(a1, "e_TerrainIntegrateObjectsMaxVertices") == 0)
		//{
		//LOG(INFO) << "e_TerrainIntegrateObjectsMaxVertices intercepted. Setting to 0.";
		//*a2 = 0;
		//}

		const auto res = big::g_hooking->get_original<hook_REGISTER_CVAR>()(a1, a2, a3, a4, a5, a6);

		return res;
	}

	char __fastcall hook_CTerrain_Load(__int64 a1, __int64 a2, int nDataSize, char *a4, __int64 **a5, __int64 *a6)
	{
		g_CTerrain = a1;

		const auto res = big::g_hooking->get_original<hook_CTerrain_Load>()(a1, a2, nDataSize, a4, a5, a6);

		return res;
	}

	bool g_inside_StepDataSBrush = false;

	char __fastcall hook_StepDataSBrush(CBrush *cbrush_, __int64 *pChunk_, __int64 pStatObjTable, __int64 pMatTable, char a5, __int64 a6, int eLoadMode)
	{
		g_inside_StepDataSBrush = true;

		const auto res = big::g_hooking->get_original<hook_StepDataSBrush>()(cbrush_, pChunk_, pStatObjTable, pMatTable, a5, a6, eLoadMode);

		g_inside_StepDataSBrush = false;

		return res;
	}

	__int64 __fastcall hook_LoadCommonData(__int64 pChunk, IRenderNode *a2, __int64 a3)
	{
		const auto res = big::g_hooking->get_original<hook_LoadCommonData>()(pChunk, a2, a3);

		if (g_inside_StepDataSBrush)
		{
			//auto matrix  = (float *)(pChunk + 40);
			//matrix[11]  += 50.0f;
		}

		return res;
	}

	static __int64 hook_CPhysicalEntity_dctor(IPhysicalEntity *inst, char a2)
	{
		const auto res = big::g_hooking->get_original<hook_CPhysicalEntity_dctor>()(inst, a2);

		g_physicalentities.erase(inst);

		return res;
	}

	__int64 hook_CPhysicalEntity_ctor(IPhysicalEntity *a1, __int64 a2, __int64 a3)
	{
		const auto res = big::g_hooking->get_original<hook_CPhysicalEntity_ctor>()(a1, a2, a3);

		std::scoped_lock l(g_cphysicalentity_mutex);

		g_physicalentities.insert(a1);

		return res;
	}

	namespace
	{
		std::vector<std::function<void()>> g_pending_hook_registrations;

		// kcd2_init_impl still describes hooks next to the signatures that feed
		// them, but this adapter defers creation of the PolyHook objects until
		// every required signature has resolved successfully.
		class transactional_hooking
		{
		public:
			class detour_hook_helper
			{
			public:
				template<auto detour_function>
				static void *add(std::string name, void *target)
				{
					g_pending_hook_registrations.emplace_back(
					    [name = std::move(name), target]
					    {
						    big::hooking::detour_hook_helper::add<detour_function>(name, target);
					    });
					return nullptr;
				}
			};
		};
	}

#define hooking transactional_hooking
	static void kcd2_init_impl()
	{
		static kcd2_address CVegetations_Ctor;
		static kcd2_address CMergedMeshRenderNode_Ctor;
		static kcd2_address CXConsole_Ctor;
		//	static kcd2_address m_p3DEngine;
		void **CentityVFTable                = nullptr;
		void **CStatObjVFTable               = nullptr;
		void **CGeomCacheRenderNodeVFTable   = nullptr;
		void **CVegetationsVFTable           = nullptr;
		void **CMergedMeshRenderNode_VFTable = nullptr;
		void **CBrush_VFTable                = nullptr;
		void **CPhysicalEntityVFTable        = nullptr;
		void **CXConsoleVFTable              = nullptr;
		void **C3DEngine_VFTable             = nullptr;

		game_lua_call = kcd2_address::resolved("lua_call");
		game_lua_checkstack = kcd2_address::resolved("lua_checkstack");
		game_lua_concat = kcd2_address::resolved("lua_concat");
		game_lua_createtable = kcd2_address::resolved("lua_createtable");
		game_lua_error = kcd2_address::resolved("lua_error");
		game_lua_gc = kcd2_address::resolved("lua_gc");
		game_lua_getfenv = kcd2_address::resolved("lua_getfenv");
		game_lua_getfield = kcd2_address::resolved("lua_getfield");
		game_lua_getmetatable = kcd2_address::resolved("lua_getmetatable");
		game_lua_gettable = kcd2_address::resolved("lua_gettable");
		game_lua_insert = kcd2_address::resolved("lua_insert");
		game_lua_pcall = kcd2_address::resolved("lua_pcall");
		game_luaV_execute = kcd2_address::resolved("luaV_execute");
		game_lua_load = kcd2_address::resolved("lua_load");
		CScriptableBase_Init_func = kcd2_address::resolved("CScriptableBase_Init");
		game_lua_setmetatable = kcd2_address::resolved("lua_setmetatable");
		lua_custom_alloc = kcd2_address::resolved("lua_custom_alloc");
		game_pushref = kcd2_address::resolved("game_pushref");
		game_index2adr = kcd2_address::resolved("game index2adr");
		game_luaH_new = kcd2_address::resolved("game luaH_new");
		g_C3DEngine_UnRegisterEntityImpl_ptr =
		    kcd2_address::resolved("C3DEngine_UnRegisterEntityImpl");
		CXConsole_Ctor = kcd2_address::resolved("CXConsole_ctor");
		CVegetations_Ctor = kcd2_address::resolved("CVegetation_ctor");
		CMergedMeshRenderNode_Ctor = kcd2_address::resolved("CMergedMeshRenderNode_ctor");

		CXConsoleVFTable = kcd2_address::derived("CXConsole vtable").as<void **>();
		g_CXConsole_pointer =
		    kcd2_address::resolved("gEnv pConsole pointer").as<uintptr_t *>();
		g_expected_CXConsole_vtable = CXConsoleVFTable;
		CVegetationsVFTable = kcd2_address::derived("CVegetation vtable").as<void **>();
		CMergedMeshRenderNode_VFTable =
		    kcd2_address::derived("CMergedMeshRenderNode vtable").as<void **>();
		CentityVFTable = kcd2_address::resolved("CEntity vtable").as<void **>();
		CStatObjVFTable = kcd2_address::resolved("CStatObj vtable").as<void **>();
		CGeomCacheRenderNodeVFTable =
		    kcd2_address::resolved("CGeomCacheRenderNode vtable").as<void **>();
		CBrush_VFTable = kcd2_address::resolved("CBrush vtable").as<void **>();
		CPhysicalEntityVFTable =
		    kcd2_address::resolved("CPhysicalEntity vtable").as<void **>();
		C3DEngine_VFTable = kcd2_address::resolved("C3DEngine vtable").as<void **>();

		{
			const auto cryengine_attachVariable =
			    kcd2_address::resolved("CryEngine attachVariable");
			if (!cryengine_attachVariable)
			{
				LOG(ERROR) << "Failed to find CryEngine attachVariable";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_attachVariable>("attachVariable hook", cryengine_attachVariable);
		}

		{
			const auto ptr = kcd2_address::resolved("LoadCommonData");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find LoadCommonData";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_LoadCommonData>("hook_LoadCommonData", ptr);
		}

		{
			const auto ptr = kcd2_address::resolved("CPhysicalEntity_ctor");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CPhysicalEntity_ctor";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_CPhysicalEntity_ctor>("hook_CPhysicalEntity_ctor", ptr);
			big::hooking::detour_hook_helper::add<hook_CPhysicalEntity_dctor>("hook_CPhysicalEntity_dctor", CPhysicalEntityVFTable[0]);
		}

		{
			const auto ptr = kcd2_address::resolved("CryEngine REGISTER_CVAR");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CryEngine REGISTER_CVAR";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_REGISTER_CVAR>("attachVariable hook", ptr);
		}

		{
			const auto init_renderer = kcd2_address::resolved("Initializing Direct3D");
			if (!init_renderer)
			{
				LOG(ERROR) << "Failed to find init_renderer";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_Initializing_Direct3D>("hook_Initializing_Direct3D", init_renderer);
		}

		{
			const auto ptr = kcd2_address::resolved("PostInputEvent");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find PostInputEvent";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_PostInputEvent>("hook_hook_PostInputEvent", ptr);
		}

		{
			const auto ptr = kcd2_address::resolved("C_UIMenu_ShowPage");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find C_UIMenu::ShowPage";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_C_UIMenu_ShowPage>(
			    "hook_C_UIMenu_ShowPage",
			    ptr);
		}

		{
			const auto ptr =
			    kcd2_address::resolved("C_SkipTimeCutscene_Play");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find C_SkipTimeCutscene::Play";
				return;
			}
			big::hooking::detour_hook_helper::add<
			    hook_C_SkipTimeCutscene_Play>(
			    "hook_C_SkipTimeCutscene_Play",
			    ptr);
		}

		{
			const auto ptr =
			    kcd2_address::resolved("C_FastTravel_StartTravel");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find C_FastTravel::StartTravel";
				return;
			}
			big::hooking::detour_hook_helper::add<
			    hook_C_FastTravel_StartTravel>(
			    "hook_C_FastTravel_StartTravel",
			    ptr);
		}

		{
			g_hook_log_write_enabled = big::config::general().bind("Logging", "Output Vanilla Game Log", false, "Output to the KCD2Online log the vanilla game log kcd.log");

			const auto ptr = kcd2_address::resolved("CLog_LogV");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CLog_LogV";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_CLog_LogV>("hook_CLog_LogV", ptr);
		}

		{
			g_noclip_enabled = big::config::general().bind("Noclip", "Enabled", false, "Toggle noclip mode on or off.");
			g_noclip_speed_default = big::config::general().bind("Noclip", "Default Speed", 0.1, "Sets the default movement speed for noclip mode.");
			g_noclip_speed_multiplier = big::config::general().bind("Noclip", "Speed Multiplier", 10.0, "Multiplier applied to speed when boosting in noclip mode.");
		}

		{
			g_show_entity_inspector = big::config::general().bind("Inspectors", "Entity", true, "Show the Entity Inspector.");
			g_show_entity_metadata_inspector = big::config::general().bind("Inspectors", "Entity Metadata", false, "Show the Entity Metadata Inspector.");
			g_show_entity_xml_infos_inspector = big::config::general().bind("Inspectors", "XML Infos", false, "Show the XML Infos Inspector.");
			g_show_cbrush_inspector = big::config::general().bind("Inspectors", "CBrush", false, "Show the CBrush Inspector.");
			g_show_CMergedMeshRenderNode_inspector = big::config::general().bind("Inspectors", "CMergedMesh", false, "Show the CMergedMesh Inspector.");
			g_show_ptf_inspector = big::config::general().bind("Inspectors", "PTF", true, "Show the PTF Inspector.");
		}

		{
			g_pending_hook_registrations.emplace_back(
			    []
			    {
				    EachImportFunction(::GetModuleHandleA("WHGame.dll"),
				                       "fmodstudio.dll",
				                       [](const char *funcname, void *&func)
				                       {
					                       if (strcmp(funcname, "?getEvent@System@Studio@FMOD@@QEBA?AW4FMOD_RESULT@@PEBDPEAPEAVEventDescription@23@@Z") == 0)
					                       {
						                       fmodstudio_getevent_orig = (fmodstudio_getevent_t)func;
						                       ForceWrite<void *>(func, hook_fmodstudio_getevent);
					                       }
					                       else if (strcmp(funcname, "?loadBankFile@System@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAPEAVBank@23@@Z") == 0)
					                       {
						                       fmodstudio_loadbankfile_orig = (fmodstudio_loadbankfile_t)func;
						                       big::ForceWrite<void *>(func, hook_fmodstudio_loadbankfile);
					                       }
					                       else if (strcmp(funcname, "?unload@Bank@Studio@FMOD@@QEAA?AW4FMOD_RESULT@@XZ") == 0)
					                       {
						                       fmodstudio_bank_unload_orig = (fmodstudio_bank_unload_t)func;
					                       }
				                       });
			    });
		}

		{
			const auto ptr = kcd2_address::resolved("XmlParserImp_ParseFile");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find XmlParserImp_ParseFile";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_XmlParserImp_ParseFile>("hook_XmlParserImp_ParseFile", ptr);
		}

		{
			const auto ptr = kcd2_address::resolved("XML_Parse");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find XML_Parse";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_XML_Parse>("hook_XML_Parse", ptr);
		}

		{
			const auto ptr = kcd2_address::resolved("CCryFile_Open");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CCryFile_Open";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_CCryFile_Open>("hook_CCryFile_Open", ptr);
		}

		{
			const auto ptr = kcd2_address::resolved("CCryPak_ctor");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CCryPak_ctor";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_CCryPak_ctor>("hook_CCryPak_ctor", ptr);
		}

		{
			const auto ptr = kcd2_address::resolved("wh_db_table_patched");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find wh_db_table_patched";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_wh_db_table_patched>("hook_wh_db_table_patched", ptr);
		}

		{
			const auto ptr = kcd2_address::resolved("wh_db_table_patch_find_line");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find wh_db_table_patch_find_line";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_wh_db_table_patch_find_line>("hook_wh_db_table_patch_find_line", ptr);
		}

		{
			const auto ptr = kcd2_address::resolved("XmlParserReadOnly_Read_caller");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find XmlParserReadOnly_Read_caller";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_XmlParserReadOnly_Read_caller>(
			    "hook_XmlParserReadOnly_Read_caller",
			    ptr);
		}

		{
			const auto ptr = kcd2_address::resolved("CXConsole_RegisterVar");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CXConsole_RegisterVar";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_CXConsole_RegisterVar>("hook_CXConsole_RegisterVar", ptr);
		}

		{
			const auto ptr = CXConsole_Ctor;
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CXConsole_Ctor";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_CXConsole_Ctor>("hook_CXConsole_Ctor", ptr);
			big::hooking::detour_hook_helper::add<hook_CXConsole_AddCommandScript>("hook_CXConsole_AddCommandScript", CXConsoleVFTable[32]);
			big::hooking::detour_hook_helper::add<hook_CXConsole_AddCommandCommand>("hook_CXConsole_AddCommandCommand", CXConsoleVFTable[33]);
		}

		{
			const auto ptr = kcd2_address::resolved("CEntitySystem_ctor");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CEntitySystem_CEntitySystem";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_CEntitySystem_CEntitySystem>("hook_CEntitySystem_CEntitySystem", ptr);
		}

		{
			const auto ptr = kcd2_address::resolved("CEntity_ctor");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CEntity_ctor";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_CEntity_ctor>("hook_CEntity_ctor", ptr);
			big::hooking::detour_hook_helper::add<hook_CEntity_dctor>("hook_CEntity_dctor", CentityVFTable[0]);
		}

		{
			const auto ptr = kcd2_address::resolved("CBrush_ctor");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CBrush_ctor";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_CBrush_ctor>("hook_CBrush_ctor", ptr);
			big::hooking::detour_hook_helper::add<hook_CBrush_dctor>("hook_CBrush_dctor", CBrush_VFTable[0]);
		}

		{
			const auto ptr = kcd2_address::resolved("CPlayerStateMovement_Ledge_callback");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find a function that uses gEnv->pGame";
				return;
			}

			g_gEnv_pGame_ptr =
			    kcd2_address::derived("gEnv pGame pointer").as<uintptr_t *>();

			// Reproduction of CryEngine SetFlyMode inside CDevMode::OnGameplayCommand
			// Doesn't work well cause of custom movement system of KCD
			//const auto gEnv_pGame = *g_gEnv_pGame_ptr;
			//const auto IGameFramework = (*(__int64(__fastcall **)(__int64))(*(uintptr_t *)gEnv_pGame + 128LL))(gEnv_pGame);
			//const auto IActor_pPlayer = (*(__int64(__fastcall **)(__int64))(*(uintptr_t *)IGameFramework + 512LL))(IGameFramework);
			//if (IActor_pPlayer)
			//{
			// IActor->SetFlyMode
			//(*(void(__fastcall **)(__int64, uint8_t))(*(uintptr_t *)IActor_pPlayer + 608LL))(IActor_pPlayer, (uint8_t)2);
			//}
		}

		{
			const auto ptr = kcd2_address::resolved("CMovableBrush_ctor");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CEntity_SetWorldTM";
				return;
			}

			g_CEntity_SetWorldTM = ptr.as<decltype(g_CEntity_SetWorldTM)>();
		}

		{
			const auto ptr = kcd2_address::resolved("StepDataSBrush");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find StepDataSBrush";
				return;
			}

			big::hooking::detour_hook_helper::add<hook_StepDataSBrush>("hook_StepDataSBrush", ptr);
		}

		{
			const auto ptr = kcd2_address::resolved("CTerrain_Load");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CTerrain_Load";
				return;
			}

			big::hooking::detour_hook_helper::add<hook_CTerrain_Load>("hook_CTerrain_Load", ptr);
		}

		{
			const auto ptr = kcd2_address::resolved("CBrush_SetStatObj");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CBrush_SetStatObj";
				return;
			}

			g_CBrush_SetStatObj = ptr.as<decltype(g_CBrush_SetStatObj)>();
		}

		{
			const auto ptr = kcd2_address::resolved("CStatObj_ctor");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CStatObj_ctor";
				return;
			}

			g_CStatObj_ctor = ptr.as<decltype(g_CStatObj_ctor)>();

			big::hooking::detour_hook_helper::add<hook_CStatObj_ctor>("hook_CStatObj_ctor", g_CStatObj_ctor);
			big::hooking::detour_hook_helper::add<hook_CStatObj_dctor>("hook_CStatObj_dctor", CStatObjVFTable[0]);
		}

		{
			const auto ptr = kcd2_address::resolved("CGeomCacheRenderNode_ctor");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CGeomCacheRenderNode_ctor";
				return;
			}

			big::hooking::detour_hook_helper::add<hook_CGeomCacheRenderNode_ctor>("hook_CGeomCacheRenderNode_ctor", ptr);
			big::hooking::detour_hook_helper::add<hook_CGeomCacheRenderNode_dctor>("hook_CGeomCacheRenderNode_dctor", CGeomCacheRenderNodeVFTable[0]);
		}

		{
			const auto ptr = CVegetations_Ctor;
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CVegetation_ctor";
				return;
			}

			big::hooking::detour_hook_helper::add<hook_CVegetation_ctor>("hook_CVegetation_ctor", ptr);
			big::hooking::detour_hook_helper::add<hook_CVegetation_dctor>("hook_CVegetation_dctor", CVegetationsVFTable[0]);
		}

		{
			const auto ptr = CMergedMeshRenderNode_Ctor;
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CMergedMeshRenderNode_ctor";
				return;
			}

			big::hooking::detour_hook_helper::add<hook_CMergedMeshRenderNode_ctor>("hook_CMergedMeshRenderNode_ctor", ptr);
			big::hooking::detour_hook_helper::add<hook_CMergedMeshRenderNode_dctor>("hook_CMergedMeshRenderNode_dctor", CMergedMeshRenderNode_VFTable[0]);
		}

		{
			const auto ptr = kcd2_address::resolved("C_PlayerStateMovement_ctor");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find C_PlayerStateMovement_ctor";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_C_PlayerStateMovement_ctor>("hook_C_PlayerStateMovement_ctor", ptr);
		}

		{
			const auto ptr = kcd2_address::resolved("C_Player_ctor");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find C_Player_ctor";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_C_Player_ctor>("hook_C_Player_ctor", ptr);
		}

		{
			const auto ptr = kcd2_address::resolved("C3DEngine_ctor");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find C3DEngine_ctor";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_C3DEngine_ctor>("hook_C3DEngine_ctor", ptr);
			big::hooking::detour_hook_helper::add<hook_C3DEngine_RegisterEntity>("hook_C3DEngine_RegisterEntity", C3DEngine_VFTable[38]);
			big::hooking::detour_hook_helper::add<hook_C3DEngine_UnRegisterEntityImpl>(
			    "hook_C3DEngine_UnRegisterEntityImpl",
			    g_C3DEngine_UnRegisterEntityImpl_ptr);
		}

		{
			const auto ptr =
			    kcd2_address::resolved("CD3D9Renderer_UnProjectFromScreen");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CD3D9Renderer_UnProjectFromScreen";
				return;
			}

			g_CD3D9Renderer_UnProjectFromScreen =
			    ptr.as<decltype(g_CD3D9Renderer_UnProjectFromScreen)>();

			big::hooking::detour_hook_helper::add<hook_CD3D9Renderer_UnProjectFromScreen>(
			    "hook_CD3D9Renderer_UnProjectFromScreen",
			    g_CD3D9Renderer_UnProjectFromScreen);
		}

		{
			const auto ptr =
			    kcd2_address::resolved("CD3D9Renderer_ProjectToScreen");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find CD3D9Renderer_ProjectToScreen";
				return;
			}

			g_CD3D9Renderer_ProjectToScreen = ptr.as<decltype(g_CD3D9Renderer_ProjectToScreen)>();
		}

		{
			const auto ptr = kcd2_address::resolved("game_lua_call_internal");
			if (!ptr)
			{
				LOG(ERROR) << "Failed to find RayWorldIntersection";
				return;
			}

			g_RayWorldIntersection = ptr.as<decltype(g_RayWorldIntersection)>();
		}

		// Early main Lua
		{
			static const TValue luaO_nilobject_ = {{NULL}, LUA_TNIL};
			static const Node dummynode_        = {
                {{NULL}, LUA_TNIL},        /* value */
                {{{NULL}, LUA_TNIL, NULL}} /* key */
            };
			luaO_nilobject_external_address = (uintptr_t)&luaO_nilobject_;
			dummynode_external_address      = (uintptr_t)&dummynode_;

			g_early_main_lua_state = luaL_newstate();

			std::scoped_lock l(lua_manager_extension::g_manager_mutex);

			lua_manager_extension::g_lua_manager_instance = std::make_unique<lua_manager>(g_early_main_lua_state,
			                                                                              g_file_manager.get_project_folder("config"),
			                                                                              g_file_manager.get_project_folder("plugins_data"),
			                                                                              g_file_manager.get_project_folder("plugins"),
			                                                                              [](sol::state_view &state, sol::table &lua_ext)
			                                                                              {
				                                                                              lua_manager_extension::init_lua_api(state, lua_ext, true);
			                                                                              });

			sol::state_view sol_state_view(g_early_main_lua_state);
			sol::table lua_ext = sol_state_view.create_named_table(rom::g_lua_api_namespace);
			lua_manager_extension::init_lua_state(sol_state_view, lua_ext, true);
			lua_manager_extension::g_lua_manager_instance->init<lua_module_ext>(false);
		}

		{
			const auto cryscriptsystem_init =
			    kcd2_address::resolved("CryScriptSystem_Init");
			if (!cryscriptsystem_init)
			{
				LOG(ERROR) << "Failed to find CryScriptSystem::Init";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_CryScriptSystem_Init>(
			    "hook_CryScriptSystem_Init",
			    cryscriptsystem_init);

			const auto lua_system_update_tick =
			    kcd2_address::resolved("CScriptSystem_Update");
			if (!lua_system_update_tick)
			{
				LOG(ERROR) << "Failed to find CryScriptSystem::Update";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_CScriptSystem_Update>("hook_lua_system_update_tick", lua_system_update_tick);

			const auto lua_execute_buffer =
			    kcd2_address::resolved("CScriptSystem_ExecuteBuffer");
			if (!lua_execute_buffer)
			{
				LOG(ERROR) << "Failed to find CryScriptSystem::ExecuteBuffer";
				return;
			}
			big::hooking::detour_hook_helper::add<hook_CScriptSystem_ExecuteBuffer>("hook_lua_execute_buffer", lua_execute_buffer);
		}

		//parse_xml_objects_file("", 0);

		char module_file_path[MAX_PATH];
		const auto path_size                    = GetModuleFileNameA(nullptr, module_file_path, MAX_PATH);
		const std::filesystem::path root_folder = std::string(module_file_path, path_size);
		const auto game_root_folder             = root_folder.parent_path().parent_path().parent_path();
		const auto game_mods_folder             = game_root_folder / "Mods";
		std::filesystem::create_directories(game_mods_folder);

		std::vector<std::string> mod_order;
		std::ifstream mod_order_file(game_mods_folder / "mod_order.txt");
		bool use_mod_order = mod_order_file.is_open();
		if (use_mod_order)
		{
			std::string line;
			while (std::getline(mod_order_file, line))
			{
				mod_order.push_back(line);
			}
		}

		// First, find all valid mod folders and their manifest
		for (const auto &entry : std::filesystem::recursive_directory_iterator(game_mods_folder, std::filesystem::directory_options::skip_permission_denied | std::filesystem::directory_options::follow_directory_symlink))
		{
			if (entry.path().filename() == "mod.manifest")
			{
				pugi::xml_document manifest_doc;
				std::string file_path = (char *)entry.path().u8string().c_str();
				if (manifest_doc.load_file(file_path.c_str()))
				{
					pugi::xml_node name_node;
					pugi::xml_node modid_node;
					pugi::xml_node description_node;
					pugi::xml_node author_node;
					pugi::xml_node version_node;
					pugi::xml_node created_on_node;
					try
					{
						name_node        = manifest_doc.select_node("//name").node();
						modid_node       = manifest_doc.select_node("//modid").node();
						description_node = manifest_doc.select_node("//description").node();
						author_node      = manifest_doc.select_node("//author").node();
						version_node     = manifest_doc.select_node("//version").node();
						created_on_node  = manifest_doc.select_node("//created_on").node();
					}
					catch (const std::exception &e)
					{
						LOG(WARNING) << e.what();
					}
					if (name_node || modid_node)
					{
						// Find the mod root folder (direct child of "Mods")
						std::filesystem::path mod_root = entry.path().parent_path();
						while (mod_root.parent_path() != game_mods_folder)
						{
							mod_root = mod_root.parent_path();
						}

						std::string mod_folder_name = big::string::to_lower((char *)mod_root.filename().u8string().c_str());
						std::string mod_id = modid_node ? big::string::to_lower(modid_node.text().as_string()) :
						                                  big::string::to_lower(name_node.text().as_string());

						LOG(INFO) << "Mapping mod folder '" << mod_folder_name << "' to modid '" << mod_id << "'";

						g_vanilla_mods.push_back({
						    .m_name        = name_node ? big::string::to_lower(name_node.text().as_string()) : mod_id,
						    .m_mod_id      = mod_id,
						    .m_description = description_node ? description_node.text().as_string() : "",
						    .m_author      = author_node ? author_node.text().as_string() : "",
						    .m_version     = version_node ? version_node.text().as_string() : "",
						    .m_folder_name = mod_folder_name,
						    .m_created_on  = created_on_node ? created_on_node.text().as_string() : "",
						});
					}
					else
					{
						LOG(ERROR) << "No modid inside " << file_path;
					}
				}
				else
				{
					LOG(ERROR) << "Invalid XML file " << (char *)entry.path().u8string().c_str();
				}
			}
		}

		// Now collect all .pak files, ensuring they are assigned to the correct mod folder
		for (const auto &entry : std::filesystem::recursive_directory_iterator(game_mods_folder, std::filesystem::directory_options::skip_permission_denied | std::filesystem::directory_options::follow_directory_symlink))
		{
			if (entry.path().extension() == ".pak")
			{
				// Find the mod root folder
				std::filesystem::path mod_root = entry.path().parent_path();
				while (mod_root.parent_path() != game_mods_folder)
				{
					mod_root = mod_root.parent_path();
				}

				std::string mod_folder_name = big::string::to_lower((char *)mod_root.filename().u8string().c_str());
				//LOG(INFO) << "mod_folder_name " << mod_folder_name;

				for (auto &vanilla_mod : g_vanilla_mods)
				{
					if (vanilla_mod.m_folder_name == mod_folder_name)
					{
						vanilla_mod.m_loaded_paks.push_back((char *)entry.path().u8string().c_str());
					}
				}
			}
		}

		if (!use_mod_order)
		{
			for (const auto &vanilla_mod : g_vanilla_mods)
			{
				for (const auto &pak_path : vanilla_mod.m_loaded_paks)
				{
					read_xmls_from_zip_file_path(pak_path.c_str());
				}
			}
			return;
		}

		// Process mods in the correct order
		for (const std::string &mod_name : mod_order)
		{
			for (const auto &mod_info : g_vanilla_mods)
			{
				if (mod_name == mod_info.m_folder_name || mod_name == mod_info.m_mod_id)
				{
					for (const auto &pak_path : mod_info.m_loaded_paks)
					{
						//LOG(INFO) << "Reading from " << (char *)pak_path.u8string().c_str();
						read_xmls_from_zip_file_path(pak_path);
					}
				}
			}
		}
	}
#undef hooking

	kcd2_init_result kcd2_init()
	{
		kcd2_address::begin_scan_session();
		g_pending_hook_registrations.clear();

		auto summary = kcd2_address::get_scan_summary();
		if (summary.failures.empty()
		    && summary.requested == summary.resolved
		    && summary.derived_requested == summary.derived_resolved)
		{
			kcd2_init_impl();
			summary = kcd2_address::get_scan_summary();
		}

		kcd2_init_result result{
		    .success              = summary.failures.empty()
		        && summary.requested == summary.resolved
		        && summary.derived_requested == summary.derived_resolved,
		    .signatures_requested = summary.requested,
		    .signatures_resolved  = summary.resolved,
		    .derived_requested    = summary.derived_requested,
		    .derived_resolved     = summary.derived_resolved,
		};

		for (const auto &failure : summary.failures)
		{
			std::string status;
			switch (failure.status)
			{
			case kcd2_address::scan_status::ambiguous:
				status = std::format("{} matches", failure.match_count);
				break;
			case kcd2_address::scan_status::invalid_pattern:
				status = "invalid pattern";
				break;
			case kcd2_address::scan_status::invalid_target:
				status = "invalid derived target";
				break;
			case kcd2_address::scan_status::module_unavailable:
				status = "WHGame.dll unavailable";
				break;
			default:
				status = "no match";
				break;
			}
			result.errors.push_back(std::format(
			    "{} ({}): {}",
			    failure.name,
			    status,
			    failure.detail.empty() ? "no diagnostic detail" : failure.detail));
		}

		if (result.success)
		{
			for (auto &register_hook : g_pending_hook_registrations)
			{
				register_hook();
			}
			if (!attach_existing_engine_console())
			{
				LOG(WARNING) << "Sandbox join will remain unavailable until CXConsole is constructed.";
			}
		}
		g_pending_hook_registrations.clear();

		return result;
	}
} // namespace big
