#include "kcse/native_runtime.hpp"
#include "kcse/join_trace.hpp"
#include "kcse/player_respawn_guard.hpp"
#include "multiplayer/profile_reconciler.hpp"
#include "multiplayer/world_catalog.hpp"

#include <REL/Module.h>
#include <REL/ID.h>
#include <crysystem/CCryAction.h>
#include <crysystem/SSystemGlobalEnvironment.h>
#include <crysystem/ScriptAnyValue.h>
#include <game/S_GameContext.h>
#include <entitymodule/C_Actor.h>
#include <guimodule/C_GUIModule.h>
#include <guimodule/C_UIBase.h>
#include <guimodule/C_UIGameOver.h>
#include <guimodule/C_UIMap.h>
#include <guimodule/C_UIMenu.h>
#include <playermodule/C_FastTravel.h>
#include <playermodule/C_MinigameManager.h>
#include <playermodule/C_PlayerModule.h>
#include <playermodule/I_Minigame.h>
#include <rpgmodule/C_Soul.h>
#include <Offsets/vtables/ICVar.h>
#include <Offsets/vtables/IConsole.h>
#include <Offsets/vtables/ISystem.h>
#include <Offsets/RTTI.h>
#include <xgenaimodule/C_AIObjectManager.h>
#include <xgenaimodule/I_AIPuppet.h>
#include <xgenaimodule/C_LinkableObject.h>
#include <xgenaimodule/C_XGenAIModule.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <format>
#include <numbers>
#include <string_view>

namespace kcd2o::kcse
{
	namespace
	{
		constexpr int environment_cvar_override_mask =
		    0x00000002 // VF_CHEAT
		    | 0x00000080 // VF_NET_SYNCED
		    | 0x00000800 // VF_READONLY
		    | 0x00800000 // VF_CONST_CVAR
		    | 0x01000000 // VF_CHEAT_ALWAYS_CHECK
		    | 0x02000000 // VF_CHEAT_NOCHECK
		    | 0x40000000; // VF_SYSSPEC_OVERWRITE

		// KCD2's CSystem state sequence ends in LEVEL_LOAD_COMPLETE (12) and
		// RUNNING (13). Player/Actor objects become visible several seconds before
		// RUNNING while character and object-layer jobs are still active, so they
		// are not a sufficient level-readiness signal on their own.
		constexpr std::uint32_t system_global_state_running = 13;

		bool read_system_global_state(std::uint32_t &state) noexcept
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			if (!environment || !environment->pSystem)
				return false;
#ifdef _WIN32
			__try
			{
#endif
				// KCD2 ISystem slot 202 is the verified GetSystemGlobalState
				// getter (mov eax,[rcx+0x2BAC]; ret) on release_1_5 build 15693.
				state = environment->pSystem->_vf202();
				return true;
#ifdef _WIN32
			}
			__except (KCD2Online_JOIN_SEH_FILTER(
			    "join.world.GetSystemGlobalState.seh"))
			{
				return false;
			}
#endif
		}

		bool in_whgame_text(const void *address) noexcept
		{
			const auto text = REL::Module::get().segment(REL::Segment::textx);
			const auto value = reinterpret_cast<std::uintptr_t>(address);
			return value >= text.address() && value - text.address() < text.size();
		}

		wh::guimodule::C_UIMenu *find_main_menu() noexcept
		{
			auto *module = wh::guimodule::C_GUIModule::GetInstance();
			if (!module)
				return nullptr;

			// Do not use C_GUIModule::GetUIElementsByName() here. That native
			// property getter constructs a game-owned std::map in caller storage;
			// moving and destroying that container in the KCSE DLL crosses module
			// allocator/CRT boundaries and has produced STATUS_HEAP_CORRUPTION at
			// join time. The module's vector is stable for the duration of this
			// game-thread callback, and a vtable comparison needs no allocation or
			// ownership transfer.
			const auto menu_vtable = REL::ID(1021).address();
			for (const auto &element : module->m_uiElements)
			{
				auto *candidate = element.get();
				if (!candidate)
					continue;
				const auto vtable = *reinterpret_cast<const std::uintptr_t *>(
				    candidate);
				if (vtable == menu_vtable)
				{
					return reinterpret_cast<wh::guimodule::C_UIMenu *>(
					    candidate);
				}
			}
			return nullptr;
		}

		wh::guimodule::C_UIMenu *guarded_find_main_menu() noexcept
		{
#ifdef _WIN32
			__try
			{
				return find_main_menu();
			}
			__except (KCD2Online_JOIN_SEH_FILTER(
			    "join.world.FindMainMenu.seh"))
			{
				return nullptr;
			}
#else
			return find_main_menu();
#endif
		}

		bool hide_game_over() noexcept
		{
			auto *module = wh::guimodule::C_GUIModule::GetInstance();
			if (!module)
				return false;

			// C_UIGameOver owns the separate black-screen fader used by the
			// initial death overlay. Closing C_UIMenu only removes the save/load
			// page; it does not release this fader because the retail flow normally
			// does that while loading a save. Multiplayer respawn has to invoke the
			// screen's native Hide path explicitly.
			const auto game_over_vtable = REL::ID(762).address();
			const auto game_over_interface_vtable = REL::ID(760).address();
			for (const auto &element : module->m_uiElements)
			{
				auto *candidate = element.get();
				if (!candidate
				    || *reinterpret_cast<const std::uintptr_t *>(candidate)
			        != game_over_vtable)
					continue;

				auto *game_over = reinterpret_cast<
				    wh::guimodule::C_UIGameOver *>(candidate);
				auto *interface = static_cast<
				    wh::playermodule::I_UIGameOver *>(game_over);
				auto **vtable = *reinterpret_cast<void ***>(interface);
				if (!vtable
				    || reinterpret_cast<std::uintptr_t>(vtable)
			        != game_over_interface_vtable
				    || !in_whgame_text(vtable[1]))
					return false;
				interface->Hide();
				return true;
			}
			return false;
		}

		bool guarded_hide_game_over() noexcept
		{
#ifdef _WIN32
			__try
			{
				return hide_game_over();
			}
			__except (KCD2Online_JOIN_SEH_FILTER(
			    "join.respawn.HideGameOver.seh"))
			{
				return false;
			}
#else
			return hide_game_over();
#endif
		}

		bool start_debug_new_game(
		    wh::guimodule::C_UISaveLoad *save_load,
		    const char *level_name) noexcept
		{
			if (!save_load || !level_name || !*level_name)
				return false;

			// The third parameter is a CryStringT<char> BY VALUE. WHGame destroys
			// that parameter in REL::ID(141236) after forwarding its character
			// buffer (Steam 0x181847140..0x181847154). Declaring it as a pointer
			// bypasses MSVC's by-value copy and makes the callee release our local
			// object, which is then released a second time by the wrapper and
			// corrupts the CryString heap during asynchronous level loading.
			using start_new_game = void(__fastcall *)(
			    wh::guimodule::C_UISaveLoad *,
			    int,
			    CryStringT<char>,
			    const char *);
			const auto address = REL::ID(141236).address();
			if (!in_whgame_text(reinterpret_cast<const void *>(address)))
				return false;
			reinterpret_cast<start_new_game>(address)(
			    save_load,
			    1,
			    CryStringT<char>{},
			    level_name);
			return true;
		}

		bool guarded_start_debug_new_game(
		    wh::guimodule::C_UISaveLoad *save_load,
		    const char *level_name) noexcept
		{
#ifdef _WIN32
			__try
			{
				return start_debug_new_game(save_load, level_name);
			}
			__except(KCD2Online_JOIN_SEH_FILTER("join.world.StartNewGame.seh"))
			{
				return false;
			}
#else
			return start_debug_new_game(save_load, level_name);
#endif
		}

#ifdef _WIN32
		bool open_main_menu() noexcept
		{
			auto *menu = find_main_menu();
			if (!menu)
				return false;
			if (menu->m_state == 1)
				return true;

			auto *interface = static_cast<wh::I_UIMenu *>(menu);
			auto **vtable = *reinterpret_cast<void ***>(interface);
			if (!vtable
			    || reinterpret_cast<std::uintptr_t>(vtable)
			        != REL::ID(1018).address()
			    || !in_whgame_text(vtable[1]))
				return false;
			interface->Open(1);
			return true;
		}

		bool guarded_open_main_menu() noexcept
		{
			__try
			{
				return open_main_menu();
			}
			__except (KCD2Online_JOIN_SEH_FILTER(
			    "join.sandbox.OpenMainMenu.seh"))
			{
				return false;
			}
		}
#else
		bool open_main_menu() noexcept
		{
			auto *menu = find_main_menu();
			if (!menu)
				return false;
			if (menu->m_state != 1)
				static_cast<wh::I_UIMenu *>(menu)->Open(1);
			return true;
		}
#endif

		bool normalize(protocol::Quaternion *rotation)
		{
			const auto length_squared =
			    rotation->x() * rotation->x()
			    + rotation->y() * rotation->y()
			    + rotation->z() * rotation->z()
			    + rotation->w() * rotation->w();
			if (!std::isfinite(length_squared)
			    || length_squared < 0.000001F)
				return false;
			const auto inverse = 1.0F / std::sqrt(length_squared);
			rotation->set_x(rotation->x() * inverse);
			rotation->set_y(rotation->y() * inverse);
			rotation->set_z(rotation->z() * inverse);
			rotation->set_w(rotation->w() * inverse);
			return true;
		}

		bool execute_console_command(
		    const char *command,
		    bool deferred = false) noexcept
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			if (!environment || !environment->pConsole)
				return false;
#ifdef _WIN32
			__try
			{
				environment->pConsole->ExecuteString(command, true, deferred);
				return true;
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
#else
			environment->pConsole->ExecuteString(command, true, deferred);
			return true;
#endif
		}

		bool execute_script(std::string_view script) noexcept
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			if (!environment || !environment->pScriptSystem || script.empty())
				return false;
#ifdef _WIN32
			__try
			{
#endif
				return environment->pScriptSystem->ExecuteBuffer(
				    script.data(),
				    script.size(),
				    "KCD2Online multiplayer rule",
				    nullptr);
#ifdef _WIN32
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
#endif
		}

		void clear_script_global(const char *name) noexcept
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			if (!environment || !environment->pScriptSystem || !name || !*name)
				return;
#ifdef _WIN32
			__try
			{
#endif
				environment->pScriptSystem->SetGlobalToNull(name);
#ifdef _WIN32
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
			}
#endif
		}

		bool take_script_global_bool(const char *name, bool &result) noexcept
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			if (!environment || !environment->pScriptSystem || !name || !*name)
				return false;
#ifdef _WIN32
			__try
			{
#endif
				ScriptAnyValue value;
				const auto found = environment->pScriptSystem->GetGlobalAny(
				    name,
				    value);
				const auto copied = found && value.CopyTo(result);
				environment->pScriptSystem->SetGlobalToNull(name);
				return copied;
#ifdef _WIN32
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
#endif
		}

		player_respawn_identity capture_player_respawn_identity(
		    const native_player_view &player) noexcept
		{
			player_respawn_identity result;
			result.entity = reinterpret_cast<std::uintptr_t>(player.entity);
			result.actor = reinterpret_cast<std::uintptr_t>(player.actor);
			if (!player.entity || !player.actor || !player.actor->m_pSoul)
				return result;
#ifdef _WIN32
			__try
			{
#endif
				result.entity_id = player.entity->GetId();
				auto *soul = player.actor->m_pSoul;
				result.soul = reinterpret_cast<std::uintptr_t>(soul);
				result.soul_guid_high = soul->m_guid.hipart;
				result.soul_guid_low = soul->m_guid.lopart;
				result.shared_soul_guid_high = soul->m_sharedSoulGuid.hipart;
				result.shared_soul_guid_low = soul->m_sharedSoulGuid.lopart;
#ifdef _WIN32
			}
			__except (KCD2Online_JOIN_SEH_FILTER(
			    "join.respawn.CapturePlayerIdentity.seh"))
			{
				return {};
			}
#endif
			return result;
		}

		std::optional<std::uint64_t> read_home_marker_wuid() noexcept
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			if (!environment || !environment->pScriptSystem)
				return std::nullopt;
			ScriptAnyValue value;
#ifdef _WIN32
			__try
			{
#endif
				const auto read = environment->pScriptSystem->GetGlobalAny(
				    "KCD2Online_HomeMarkerWUID", value);
				environment->pScriptSystem->SetGlobalToNull(
				    "KCD2Online_HomeMarkerWUID");
				if (read && value.type == ANY_THANDLE && value.nHandle != 0)
					return static_cast<std::uint64_t>(value.nHandle);
#ifdef _WIN32
			}
			__except(KCD2Online_JOIN_SEH_FILTER("home-marker.read-wuid.seh"))
			{
				return std::nullopt;
			}
#endif
			return std::nullopt;
		}

		wh::xgenaimodule::C_LinkableObject *find_linkable_object_by_wuid(
		    std::uint64_t wuid) noexcept
		{
			if (wuid == 0)
				return nullptr;
#ifdef _WIN32
			__try
			{
#endif
				auto *module = wh::xgenaimodule::C_XGenAIModule::GetInstance();
				auto *manager = module && module->m_pSingletons
				    ? static_cast<wh::xgenaimodule::C_AIObjectManager *>(
				          module->m_pSingletons->GetAIObjectManager())
				    : nullptr;
				if (!manager)
					return nullptr;
				const auto found = manager->m_objects.find(
				    wh::framework::WUID{wuid});
				if (found == manager->m_objects.end() || !found->second)
					return nullptr;
				return kcd_cast<wh::xgenaimodule::C_LinkableObject *>(
				    found->second);
#ifdef _WIN32
			}
			__except(KCD2Online_JOIN_SEH_FILTER("home-marker.resolve-linkable.seh"))
			{
				return nullptr;
			}
#endif
		}

		wh::xgenaimodule::C_LinkableObject *find_linkable_object(
		    std::uint32_t entity_id)
		{
			if (entity_id == 0)
				return nullptr;

			// Ask the game's script binding for the exact WUID owned by this
			// streamed entity.  The old implementation walked every AI object on
			// every frame and called through each puppet/host vtable.  Besides being
			// O(all AI objects), that raced object streaming and could dereference a
			// half-destroyed situation-area object when the player approached home.
			const auto query = std::format(
			    "KCD2Online_HomeMarkerWUID=nil; do local e=System.GetEntity({}); "
			    "if e and XGenAIModule and XGenAIModule.GetMyWUID then "
			    "local ok,w=pcall(XGenAIModule.GetMyWUID,e); "
			    "if ok then KCD2Online_HomeMarkerWUID=w end end end",
			    entity_id);
			if (!execute_script(query))
				return nullptr;
			const auto wuid = read_home_marker_wuid();
			return wuid ? find_linkable_object_by_wuid(*wuid) : nullptr;
		}

		struct home_marker_apply_result
		{
			wh::guimodule::C_UIMap *map{};
			std::shared_ptr<wh::guimodule::S_EntityMapMark> mark;
			std::uint32_t entity_id{};
			bool filter_was_visible{};
		};

		bool apply_home_marker_native(
		    const protocol::PropertyHomeMarker &marker,
		    home_marker_apply_result &result)
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			if (!environment || !environment->pEntitySystem)
				return false;
			result.entity_id = environment->pEntitySystem->FindEntityByGuid(
			    marker.entity_guid());
			auto *object = find_linkable_object(result.entity_id);
			result.map = wh::guimodule::C_UIMap::GetInstance();
			if (!object || !result.map)
				return false;
			result.map->GetOrCreateEntityMark(
			    result.mark,
			    wh::guimodule::E_MarkType::Home,
			    object,
			    2);
			if (!result.mark)
				return false;
			result.filter_was_visible = result.map->IsPoiFilterVisible(
			    wh::guimodule::E_MarkType::Home);
			result.map->RegisterEntityMark(result.mark);
			if (!result.filter_was_visible)
				result.map->SetPoiMarkersVisible(
				    wh::guimodule::E_MarkType::Home, true);
			return true;
		}

		bool guarded_apply_home_marker(
		    const protocol::PropertyHomeMarker &marker,
		    home_marker_apply_result &result) noexcept
		{
#ifdef _WIN32
			__try
			{
				return apply_home_marker_native(marker, result);
			}
			__except(KCD2Online_JOIN_SEH_FILTER("home-marker.apply.seh"))
			{
				return false;
			}
#else
			return apply_home_marker_native(marker, result);
#endif
		}

		void remove_home_entity_mark(
		    wh::guimodule::C_UIMap *map,
		    std::shared_ptr<wh::guimodule::S_EntityMapMark> &mark)
		{
			// Native C_UIMap::RemoveEntityMark, the same release path used by
			// C_ShowMapMarker::Untrigger.
			using function = void(__fastcall *)(
			    wh::guimodule::C_UIMap *,
			    std::shared_ptr<wh::guimodule::S_EntityMapMark> *);
			static REL::Relocation<function> native{REL::ID(55367)};
			native(map, &mark);
		}

		std::string lua_string(std::string_view value)
		{
			std::string result{"\""};
			result.reserve(value.size() + 2);
			for (const auto character : value)
			{
				if (character == '\\' || character == '\"')
					result.push_back('\\');
				result.push_back(character);
			}
			result.push_back('\"');
			return result;
		}

		bool environment_runtime_available() noexcept
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			if (!environment || !environment->pConsole)
				return false;
#ifdef _WIN32
			__try
			{
#endif
				for (const auto *name : {"e_TimeOfDay", "e_TimeOfDaySpeed"})
				{
					auto *cvar = environment->pConsole->GetCVar(name);
					if (!cvar)
						return false;
					auto **vtable = *reinterpret_cast<void ***>(cvar);
					if (!vtable)
						return false;
					for (const auto slot : {4U, 10U, 11U, 12U, 13U})
					{
						if (!vtable[slot] || !in_whgame_text(vtable[slot]))
							return false;
					}
				}
				return true;
#ifdef _WIN32
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
#endif
		}

		bool guarded_force_set_cvar(
		    ICVar *cvar,
		    const char *text,
		    float *actual,
		    int *original_flags,
		    int *overridden_flags) noexcept
		{
			*original_flags = 0;
			*overridden_flags = 0;
#ifdef _WIN32
			__try
			{
				__try
				{
#endif
					auto **vtable = *reinterpret_cast<void ***>(cvar);
					if (!vtable)
						return false;
					for (const auto slot : {4U, 10U, 11U, 12U, 13U})
					{
						if (!vtable[slot] || !in_whgame_text(vtable[slot]))
							return false;
					}
					*original_flags = cvar->GetFlags();
					*overridden_flags =
					    *original_flags & environment_cvar_override_mask;
					if (*overridden_flags != 0)
						cvar->ClearFlags(*overridden_flags);
					using force_set = void(__fastcall *)(ICVar *, const char *);
					reinterpret_cast<force_set>(vtable[10])(cvar, text);
					*actual = cvar->GetFVal();
#ifdef _WIN32
				}
				__finally
				{
#endif
					if (*overridden_flags != 0)
					{
						const auto current_flags = cvar->GetFlags();
						cvar->SetFlags(current_flags | *overridden_flags);
					}
#ifdef _WIN32
				}
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
#endif
			return true;
		}

		bool force_set_environment_cvar(
		    const char *name,
		    double value,
		    bool circular,
		    std::string &error) noexcept
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			if (!environment || !environment->pConsole)
			{
				error = "native engine console is unavailable";
				return false;
			}
			auto *cvar = environment->pConsole->GetCVar(name);
			if (!cvar)
			{
				error = std::format("required environment CVar '{}' is unavailable", name);
				return false;
			}
			const auto text = std::format("{:.6f}", value);
			float actual_float{};
			int original_flags{};
			int overridden_flags{};
			if (!guarded_force_set_cvar(
			        cvar,
			        text.c_str(),
			        &actual_float,
			        &original_flags,
			        &overridden_flags))
			{
				error = std::format(
				    "CVar '{}' override raised an SEH exception or has an "
				    "incompatible vtable",
				    name);
				KCD2Online_JOIN_TRACE("join.environment.cvar.failed", error);
				return false;
			}
			const auto actual = static_cast<double>(actual_float);
			const auto difference = circular
			    ? circular_time_distance_hours(actual, value)
			    : std::abs(actual - value);
			if (!std::isfinite(actual) || difference > 0.001)
			{
				error = std::format(
				    "CVar '{}' rejected override to {}; actual value is {}; "
				    "flags=0x{:08X}; overridden=0x{:08X}",
				    name,
				    value,
				    actual,
				    static_cast<unsigned int>(original_flags),
				    static_cast<unsigned int>(overridden_flags));
				KCD2Online_JOIN_TRACE("join.environment.cvar.failed", error);
				return false;
			}
			KCD2Online_JOIN_TRACE(
			    "join.environment.cvar.applied",
			    std::format(
			        "name=\"{}\" requested={} actual={} flags=0x{:08X} "
			        "overridden=0x{:08X}",
			        name,
			        value,
			        actual,
			        static_cast<unsigned int>(original_flags),
			        static_cast<unsigned int>(overridden_flags)));
			return true;
		}

		protocol::Quaternion quaternion_from_matrix(const Matrix34 &matrix)
		{
			protocol::Quaternion result;
			const auto trace = matrix.m00 + matrix.m11 + matrix.m22;
			if (trace > 0.0F)
			{
				const auto scale = std::sqrt(trace + 1.0F) * 2.0F;
				result.set_w(0.25F * scale);
				result.set_x((matrix.m21 - matrix.m12) / scale);
				result.set_y((matrix.m02 - matrix.m20) / scale);
				result.set_z((matrix.m10 - matrix.m01) / scale);
			}
			else if (matrix.m00 > matrix.m11 && matrix.m00 > matrix.m22)
			{
				const auto scale =
				    std::sqrt(1.0F + matrix.m00 - matrix.m11 - matrix.m22)
				    * 2.0F;
				result.set_w((matrix.m21 - matrix.m12) / scale);
				result.set_x(0.25F * scale);
				result.set_y((matrix.m01 + matrix.m10) / scale);
				result.set_z((matrix.m02 + matrix.m20) / scale);
			}
			else if (matrix.m11 > matrix.m22)
			{
				const auto scale =
				    std::sqrt(1.0F + matrix.m11 - matrix.m00 - matrix.m22)
				    * 2.0F;
				result.set_w((matrix.m02 - matrix.m20) / scale);
				result.set_x((matrix.m01 + matrix.m10) / scale);
				result.set_y(0.25F * scale);
				result.set_z((matrix.m12 + matrix.m21) / scale);
			}
			else
			{
				const auto scale =
				    std::sqrt(1.0F + matrix.m22 - matrix.m00 - matrix.m11)
				    * 2.0F;
				result.set_w((matrix.m10 - matrix.m01) / scale);
				result.set_x((matrix.m02 + matrix.m20) / scale);
				result.set_y((matrix.m12 + matrix.m21) / scale);
				result.set_z(0.25F * scale);
			}
			if (!normalize(&result))
			{
				result.Clear();
				result.set_w(1.0F);
			}
			return result;
		}

		bool replayable_non_combat_fragment(std::string_view fragment)
		{
			if (fragment.empty() || fragment.size() > 96)
				return false;
			if (!std::ranges::all_of(
			        fragment,
			        [](unsigned char character)
			        {
				        return std::isalnum(character) != 0
				            || character == '_' || character == '-'
				            || character == '/' || character == '.'
				            || character == ':';
			        }))
				return false;
			std::string lower(fragment);
			std::ranges::transform(
			    lower,
			    lower.begin(),
			    [](unsigned char value)
			    {
				    return static_cast<char>(std::tolower(value));
			    });
			constexpr std::string_view excluded[] = {
			    "combat", "attack", "strike", "parry", "block", "hit",
			    "death", "finisher", "motion", "locomotion", "idle",
			    "walk", "run", "sprint"};
			return std::ranges::none_of(
			    excluded,
			    [&](std::string_view token)
			    {
				    return lower.contains(token);
			    });
		}

		protocol::Vec3 facing_from_rotation(
		    const protocol::Quaternion &rotation)
		{
			protocol::Vec3 result;
			result.set_x(2.0F * (rotation.x() * rotation.y()
			                         - rotation.w() * rotation.z()));
			result.set_y(1.0F - 2.0F * (rotation.x() * rotation.x()
			                            + rotation.z() * rotation.z()));
			result.set_z(2.0F * (rotation.y() * rotation.z()
			                         + rotation.w() * rotation.x()));
			return result;
		}

		std::uint64_t now_ms()
		{
			return static_cast<std::uint64_t>(
			    std::chrono::duration_cast<std::chrono::milliseconds>(
			        std::chrono::steady_clock::now().time_since_epoch())
			        .count());
		}

	}

	native_runtime::native_runtime(const KCSE::IKCSEInterface &kcse) :
	    m_kcse(kcse),
	    m_profiles(m_entities),
	    m_remote_backend(m_entities),
	    m_remote_avatars(m_remote_backend)
	{
		const auto &address_library = REL::IDDatabase::get().metadata();
		m_address_library = address_library.build_key;
		m_address_library_distribution = std::string(
		    REL::to_string(address_library.distribution));
		std::ranges::transform(
		    m_address_library_distribution,
		    m_address_library_distribution.begin(),
		    [](unsigned char value)
		    {
			    return static_cast<char>(std::tolower(value));
		    });
		m_address_library_format = address_library.format_version;
		m_address_library_entries = address_library.entry_count;
		m_address_library_sha256 = address_library.sha256;
		std::scoped_lock lock(m_cache_mutex);
		m_capabilities = runtime_capability_kcse;
		m_diagnostic =
		    "Waiting for KCSE PostUpdate; no savegame is required.";
	}

	void native_runtime::on_lifecycle(std::uint32_t message_type) noexcept
	{
		switch (message_type)
		{
		case KCSE::IMessagingInterface::kMessage_DataLoaded:
			m_data_loaded.store(true, std::memory_order_release);
			m_world_lifecycle_seen.store(true, std::memory_order_release);
			m_epoch_invalidated.store(true, std::memory_order_release);
			break;
		case KCSE::IMessagingInterface::kMessage_PreDataLoaded:
			m_data_loaded.store(false, std::memory_order_release);
			m_world_pre_data_seen.store(true, std::memory_order_release);
			m_world_lifecycle_seen.store(true, std::memory_order_release);
			m_epoch_invalidated.store(true, std::memory_order_release);
			break;
		case KCSE::IMessagingInterface::kMessage_LoadGame:
		case KCSE::IMessagingInterface::kMessage_NewGame:
			m_world_lifecycle_seen.store(true, std::memory_order_release);
			m_epoch_invalidated.store(true, std::memory_order_release);
			break;
		default:
			break;
		}
	}

	bool native_runtime::on_frame()
	{
		KCD2Online_JOIN_TRACE(
		    "join.kcse.frame.begin",
		    std::format(
		        "epoch={} invalidated={} data_loaded={}",
		        m_epoch.load(std::memory_order_acquire),
		        m_epoch_invalidated.load(std::memory_order_acquire),
		        m_data_loaded.load(std::memory_order_acquire)));
		m_frame_seen.store(true, std::memory_order_release);
		m_entities.process_pending_entity_control();
		m_remote_backend.advance_frame();
		const auto changed =
		    m_epoch_invalidated.exchange(false, std::memory_order_acq_rel);
		if (changed)
			invalidate_epoch_on_game_thread();
		bool unload_transition{};
		{
			std::scoped_lock lock(m_cache_mutex);
			unload_transition = m_unload_pending || m_main_menu_pending;
		}
		refresh_cached_state();
		refresh_home_marker();
		poll_local_activity();
		advance_native_world_start();
		finish_native_unload_if_complete();
		open_main_menu_if_pending();
		KCD2Online_JOIN_TRACE(
		    "join.kcse.frame.complete",
		    std::format(
		        "epoch_changed={} unload_transition={}",
		        changed,
		        unload_transition));
		return changed && !unload_transition;
	}

	void native_runtime::on_blacksmithing_started(
	    std::uint32_t station_entity_id)
	{
		if (!sandbox_active() || station_entity_id == 0)
			return;
		auto *environment = SSystemGlobalEnvironment::GetInstance();
		auto *station = environment && environment->pEntitySystem
		    ? environment->pEntitySystem->GetEntity(station_entity_id)
		    : nullptr;
		if (!station || station->GetGuid() == 0)
			return;
		m_pending_activity_start = local_activity_start{
		    protocol::PLAYER_ACTIVITY_KIND_BLACKSMITHING,
		    station->GetGuid()};
		m_native_activity_kind =
		    protocol::PLAYER_ACTIVITY_KIND_BLACKSMITHING;
		m_activity_end_pending = false;
		KCD2Online_JOIN_TRACE(
		    "activity.blacksmithing.started",
		    std::format(
		        "station_entity_id={} station_guid={}",
		        station_entity_id,
		        station->GetGuid()));
	}

	std::optional<local_activity_start>
	native_runtime::take_local_activity_start()
	{
		auto result = std::move(m_pending_activity_start);
		m_pending_activity_start.reset();
		return result;
	}

	bool native_runtime::local_activity_end_pending() const noexcept
	{
		return m_activity_end_pending;
	}

	void native_runtime::acknowledge_local_activity_end() noexcept
	{
		m_activity_end_pending = false;
	}

	void native_runtime::cancel_local_activity()
	{
		if (auto *session = find_local_minigame(m_native_activity_kind))
			session->RequestExit();
		m_pending_activity_start.reset();
		m_native_activity_kind = protocol::PLAYER_ACTIVITY_KIND_NONE;
		m_activity_end_pending = false;
	}

	wh::playermodule::I_Minigame *native_runtime::find_local_minigame(
	    protocol::PlayerActivityKind kind) const
	{
		wh::playermodule::E_MinigameType::Type native_kind{};
		switch (kind)
		{
		case protocol::PLAYER_ACTIVITY_KIND_BLACKSMITHING:
			native_kind = wh::playermodule::E_MinigameType::Blacksmithing;
			break;
		case protocol::PLAYER_ACTIVITY_KIND_SHARPENING:
			native_kind = wh::playermodule::E_MinigameType::Sharpening;
			break;
		case protocol::PLAYER_ACTIVITY_KIND_ALCHEMY:
			native_kind = wh::playermodule::E_MinigameType::Alchemy;
			break;
		case protocol::PLAYER_ACTIVITY_KIND_NONE:
		default:
			return nullptr;
		}
		auto *context = wh::game::S_GameContext::GetInstance();
		auto player = m_entities.player();
		if (!context || !context->m_pPlayerModule
		    || !context->m_pPlayerModule->m_pMinigameManager
		    || !player.entity)
		{
			return nullptr;
		}
		return context->m_pPlayerModule->m_pMinigameManager
		    ->FindOrCreateSession(
		        player.entity->GetId(),
		        native_kind,
		        false,
		        false);
	}

	void native_runtime::poll_local_activity()
	{
		if (m_native_activity_kind == protocol::PLAYER_ACTIVITY_KIND_NONE
		    || m_activity_end_pending)
		{
			return;
		}
		auto *session = find_local_minigame(m_native_activity_kind);
		if (session && !session->IsFinished())
			return;
		KCD2Online_JOIN_TRACE(
		    "activity.local.finished",
		    std::format(
		        "kind={}",
		        static_cast<int>(m_native_activity_kind)));
		m_native_activity_kind = protocol::PLAYER_ACTIVITY_KIND_NONE;
		m_activity_end_pending = true;
	}

	runtime_descriptor native_runtime::descriptor() const
	{
		std::scoped_lock lock(m_cache_mutex);
		return {
		    m_capabilities | known_client_runtime_capabilities,
		    m_kcse.GetKCSEVersion(),
		    m_kcse.GetGameVersion(),
		    m_kcse.GetReleaseIndex(),
		    m_epoch.load(std::memory_order_acquire),
		    m_address_library,
		    m_address_library_distribution,
		    m_address_library_format,
		    m_address_library_entries,
		    m_address_library_sha256};
	}

	runtime_gate native_runtime::capability() const
	{
		std::scoped_lock lock(m_cache_mutex);
		if (!m_multiplayer_requested.load(std::memory_order_acquire))
		{
			return {
			    false,
			    false,
			    "Multiplayer runtime is idle; click Connect to initialize it."};
		}
		const auto address_library_ready = !m_address_library.empty()
		    && !m_address_library_distribution.empty()
		    && m_address_library_format != 0
		    && m_address_library_entries != 0
		    && m_address_library_sha256.size() == 64;
		if (m_frame_seen.load(std::memory_order_acquire)
		    && address_library_ready && !m_unload_pending)
			return {true, false, {}};
		return {
		    false,
		    true,
		    "Waiting for the KCSE game-thread and Address Library bootstrap."};
	}

	bool native_runtime::can_start_join() const
	{
		std::scoped_lock lock(m_cache_mutex);
		return m_frame_seen.load(std::memory_order_acquire)
		    && !m_address_library.empty()
		    && !m_address_library_distribution.empty()
		    && m_address_library_format != 0
		    && m_address_library_entries != 0
		    && m_address_library_sha256.size() == 64
		    && !m_unload_pending;
	}

	bool native_runtime::prepare_multiplayer()
	{
		KCD2Online_JOIN_TRACE(
		    "join.runtime.prepare.precheck",
		    std::format(
		        "data_loaded={} frame_seen={} can_start_join={}",
		        m_data_loaded.load(std::memory_order_acquire),
		        m_frame_seen.load(std::memory_order_acquire),
		        can_start_join()));
		if (!can_start_join())
		{
			std::scoped_lock lock(m_cache_mutex);
			m_diagnostic =
			    "KCSE has not reached a safe game-thread frame yet.";
			KCD2Online_JOIN_TRACE(
			    "join.runtime.prepare.rejected",
			    m_diagnostic);
			return false;
		}
		m_multiplayer_requested.store(true, std::memory_order_release);
		{
			std::scoped_lock lock(m_cache_mutex);
			m_diagnostic = m_local_transform
			    ? "Initializing native multiplayer runtime capabilities."
			    : "Connecting before world creation; awaiting server bootstrap.";
		}
		KCD2Online_JOIN_TRACE(
		    "join.runtime.prepare.requested",
		    std::format(
		        "epoch={}",
		        m_epoch.load(std::memory_order_acquire)));
		return true;
	}

	void native_runtime::cancel_multiplayer_preparation()
	{
		if (!m_multiplayer_requested.exchange(false, std::memory_order_acq_rel)
		    && !m_preparation_active)
			return;
		remove_home_marker();
		m_home_marker.reset();
		m_remote_avatars.clear();
		m_remote_backend.clear();
		m_remote_backend.reset_active_probe();
		m_profiles.reset();
		m_preparation_active = false;
		m_preparation_frames = 0;
		m_probe_transform_verified = false;
		m_probe_complete = false;
		m_probe_failed = false;
		m_probe_error.clear();
		m_expected_epoch_transition.store(false, std::memory_order_release);
		m_world_lifecycle_seen.store(false, std::memory_order_release);
		m_world_pre_data_seen.store(false, std::memory_order_release);
		m_transition_safe = false;
		m_transition_blocker.clear();
		std::scoped_lock lock(m_cache_mutex);
		m_world_start_bootstrap.reset();
		m_world_start_level_id.clear();
		m_world_start_level_name.clear();
		m_world_start_requires_lifecycle = false;
		m_world_start_stage = world_start_stage::idle;
		if (!m_sandbox_active && !m_unload_pending)
		{
			m_sandbox_progress = {};
			m_diagnostic =
			    "Multiplayer runtime is idle; click Connect to initialize it.";
		}
	}

	sandbox_start_result native_runtime::begin_sandbox(
	    const protocol::ServerBootstrap &bootstrap)
	{
		const auto target = find_native_world_level(bootstrap.level_id());
		if (!target)
		{
			return {
			    false,
			    "Server level '" + bootstrap.level_id()
			        + "' is not registered in the native KCD2 level catalog."};
		}

		bool loaded_target{};
		bool has_player{};
		{
			std::scoped_lock lock(m_cache_mutex);
			loaded_target = m_data_loaded.load(std::memory_order_acquire)
			    && m_level_id == target->id;
			has_player = m_local_transform.has_value();
		}
		if (loaded_target && has_player)
		{
			std::uint64_t capabilities{};
			{
				std::scoped_lock lock(m_cache_mutex);
				capabilities = m_capabilities;
			}
			if ((required_client_runtime_capabilities & ~capabilities) == 0)
				return activate_loaded_sandbox(bootstrap);

			std::scoped_lock lock(m_cache_mutex);
			m_world_start_bootstrap = bootstrap;
			m_world_start_level_id = std::string(target->id);
			m_world_start_level_name = std::string(target->name);
			m_world_start_requires_lifecycle = false;
			m_world_start_stage = world_start_stage::probing_runtime;
			m_sandbox_progress = {};
			m_sandbox_progress.phase = sandbox_phase::loading;
			m_diagnostic =
			    "Loaded target world is waiting for native capability validation.";
			KCD2Online_JOIN_TRACE(
			    "join.world.loaded-probe",
			    std::format(
			        "target_level={} missing=0x{:X}",
			        target->id,
			        required_client_runtime_capabilities & ~capabilities));
			return {true, {}};
		}
		if (has_player)
		{
			return {
			    false,
			    "A different native level is already active. Return to the title "
			    "screen before joining; synchronized live travel is not enabled yet."};
		}
		return begin_native_world_start(bootstrap);
	}

	sandbox_start_result native_runtime::activate_loaded_sandbox(
	    const protocol::ServerBootstrap &bootstrap)
	{
		KCD2Online_JOIN_TRACE(
		    "join.sandbox.native.begin",
		    std::format(
		        "session_id=\"{}\" requested_level=\"{}\" mode={} "
		        "profile={} spawn_valid={} profile_transform_valid={} "
		        "profile_has_transform={} current_level=\"{}\" "
		        "capabilities=0x{:X}",
		        bootstrap.session_id(),
		        bootstrap.level_id(),
		        static_cast<int>(bootstrap.mode()),
		        bootstrap.has_profile(),
		        bootstrap.spawn_valid(),
		        bootstrap.has_profile()
		            && bootstrap.profile().transform_valid(),
		        bootstrap.has_profile()
		            && bootstrap.profile().has_last_transform(),
		        current_level_id(),
		        descriptor().capabilities));
		std::unique_lock lock(m_cache_mutex);
		if (!m_local_transform)
		{
			KCD2Online_JOIN_TRACE(
			    "join.sandbox.native.rejected",
			    "local transform is nil");
			return {false, "Native player is not ready."};
		}
		const auto requested_level = canonical_level_id(bootstrap.level_id());
		if (requested_level.empty() || requested_level != m_level_id)
		{
			KCD2Online_JOIN_TRACE(
			    "join.sandbox.native.rejected",
			    std::format(
			        "level mismatch requested=\"{}\" current=\"{}\"",
			        bootstrap.level_id(),
			        m_level_id));
			return {false, "Loaded native world does not match the server level."};
		}
		if ((m_capabilities & runtime_capability_profile_apply) == 0
		    || !bootstrap.has_profile())
		{
			KCD2Online_JOIN_TRACE(
			    "join.sandbox.native.rejected",
			    std::format(
			        "profile_apply_capability={} bootstrap_profile={}",
			        (m_capabilities & runtime_capability_profile_apply) != 0,
			        bootstrap.has_profile()));
			return {
			    false,
			    "Native profile application or authoritative server profile "
			    "is unavailable."};
		}

		const auto selected_spawn =
		    select_sandbox_spawn(bootstrap, m_local_transform);
		if (!selected_spawn.transform)
		{
			KCD2Online_JOIN_TRACE(
			    "join.sandbox.spawn-selection.failed",
			    std::format(
			        "mode={} profile_transform_valid={} "
			        "profile_has_transform={} spawn_valid={} has_spawn={} "
			        "local_transform={}",
			        static_cast<int>(bootstrap.mode()),
			        bootstrap.profile().transform_valid(),
			        bootstrap.profile().has_last_transform(),
			        bootstrap.spawn_valid(),
			        bootstrap.has_spawn(),
			        m_local_transform.has_value()));
			return {
			    false,
			    "Server bootstrap contains no usable spawn transform."};
		}
		const auto &spawn = *selected_spawn.transform;
		const auto spawn_source = to_string(selected_spawn.source);
		KCD2Online_JOIN_TRACE(
		    "join.sandbox.spawn-selection.ok",
		    std::format(
		        "source={} position=({:.6f},{:.6f},{:.6f}) "
		        "rotation=({:.6f},{:.6f},{:.6f},{:.6f})",
		        spawn_source,
		        spawn.position().x(),
		        spawn.position().y(),
		        spawn.position().z(),
		        spawn.rotation().x(),
		        spawn.rotation().y(),
		        spawn.rotation().z(),
		        spawn.rotation().w()));
		if (!is_finite_transform(spawn))
		{
			KCD2Online_JOIN_TRACE(
			    "join.sandbox.spawn-selection.invalid",
			    std::format("source={}", spawn_source));
			return {
			    false,
			    "Selected sandbox spawn transform is invalid."};
		}

		auto target = bootstrap.profile();
		target.set_transform_valid(true);
		*target.mutable_last_transform() = spawn;
		if (!is_valid_profile(target))
		{
			KCD2Online_JOIN_TRACE(
			    "join.sandbox.target-profile.invalid",
			    std::format(
			        "source={} player_id={} revision={} level=\"{}\" "
			        "stats={} skills={} inventory={} avatar={} "
			        "transform_valid={} has_transform={}",
			        spawn_source,
			        target.player_id(),
			        target.revision(),
			        target.level_id(),
			        target.stats_size(),
			        target.skills_size(),
			        target.inventory_size(),
			        target.has_avatar(),
			        target.transform_valid(),
			        target.has_last_transform()));
			return {
			    false,
			    "Authoritative target profile is invalid after spawn "
			    "selection."};
		}
		KCD2Online_JOIN_TRACE(
		    "join.sandbox.target-profile.valid",
		    std::format(
		        "player_id={} revision={} source={}",
		        target.player_id(),
		        target.revision(),
		        spawn_source));

		auto *framework = CCryAction::GetInstance();
		if (!framework)
		{
			KCD2Online_JOIN_TRACE(
			    "join.sandbox.native.rejected",
			    "CCryAction=nil");
			return {false, "CCryAction is unavailable."};
		}
		lock.unlock();
		const auto environment_applied =
		    apply_environment_state(bootstrap.environment(), true);
		lock.lock();
		if (!environment_applied)
		{
			const auto environment_error = m_diagnostic.empty()
			    ? std::string{"Native server environment bootstrap failed."}
			    : m_diagnostic;
			KCD2Online_JOIN_TRACE(
			    "join.sandbox.environment.failed",
			    environment_error);
			return {false, environment_error};
		}
		KCD2Online_JOIN_TRACE(
		    "join.sandbox.environment.ok",
		    std::format(
		        "time_of_day={} time_scale={} weather_id={} "
		        "weather_transition_ms={}",
		        bootstrap.environment().time_of_day_hours(),
		        bootstrap.environment().time_scale(),
		        bootstrap.environment().weather_id(),
		        bootstrap.environment().weather_transition_ms()));
		KCD2Online_JOIN_TRACE(
		    "join.sandbox.save-load-lock.begin",
		    std::format(
		        "framework={}",
		        static_cast<void *>(framework)));
		framework->AllowSave(false);
		framework->AllowLoad(false);
		m_save_load_locked = true;
		m_profiles.set_wire_identity(target);
		const auto applied = reconcile_profile(m_profiles, target);
		KCD2Online_JOIN_TRACE(
		    applied.success
		        ? "join.sandbox.profile-apply.ok"
		        : "join.sandbox.profile-apply.failed",
		    std::format(
		        "success={} rollback_attempted={} rollback_succeeded={} "
		        "requires_world_unload={} error=\"{}\"",
		        applied.success,
		        applied.rollback_attempted,
		        applied.rollback_succeeded,
		        profile_failure_requires_world_unload(applied),
		        applied.error));
		if (!applied.success)
		{
			m_profiles.reset();
			if (profile_failure_requires_world_unload(applied))
			{
				lock.unlock();
				begin_native_unload(
				    "Native profile rollback failed; unloading the modified "
				    "save.");
			}
			else
			{
				restore_save_load();
			}
			return {
			    false,
			    "Native profile transaction failed: " + applied.error};
		}
		std::string world_error;
		if (!m_entities.begin_world_sync(world_error))
		{
			m_profiles.reset();
			lock.unlock();
			begin_native_unload(world_error);
			return {false, world_error};
		}
		(void)execute_console_command(
		    "cheat_no_lockpicking nolockpicks:true");
		(void)execute_console_command("cheat_own_stolen_items");
		for (const auto &object : bootstrap.world_objects())
		{
			if (!m_entities.apply_world_object_state(object, world_error))
			{
				m_profiles.reset();
				lock.unlock();
				begin_native_unload(world_error);
				return {
				    false,
				    "Native world-object bootstrap failed: " + world_error};
			}
		}
		for (const auto &item : bootstrap.world_items())
		{
			if (!m_entities.apply_world_item_state(item, world_error))
			{
				m_profiles.reset();
				lock.unlock();
				begin_native_unload(world_error);
				return {
				    false,
				    "Native world-item bootstrap failed: " + world_error};
			}
		}
		m_sandbox_active = true;
		const auto map_reveal_dispatched =
		    execute_console_command("player_revealFow");
		KCD2Online_JOIN_TRACE(
		    map_reveal_dispatched
		        ? "join.sandbox.map-reveal.ok"
		        : "join.sandbox.map-reveal.failed",
		    map_reveal_dispatched
		        ? "The multiplayer map fog was cleared."
		        : "The multiplayer map fog reveal command could not be dispatched.");
		(void)execute_script(
		    "if player and player.EnableFastTravel then "
		    "player:EnableFastTravel(false) end");
		m_sandbox_progress.phase = sandbox_phase::ready;
		m_sandbox_progress.initial_spawn = spawn;
		m_world_start_bootstrap.reset();
		m_world_start_level_id.clear();
		m_world_start_level_name.clear();
		m_world_start_requires_lifecycle = false;
		m_world_start_stage = world_start_stage::idle;
		m_expected_epoch_transition.store(false, std::memory_order_release);
		KCD2Online_JOIN_TRACE(
		    "join.sandbox.native.ready",
		    std::format(
		        "spawn=({:.6f},{:.6f},{:.6f})",
		        spawn.position().x(),
		        spawn.position().y(),
		        spawn.position().z()));
		return {true, {}};
	}

	sandbox_start_result native_runtime::begin_native_world_start(
	    const protocol::ServerBootstrap &bootstrap)
	{
		const auto target = find_native_world_level(bootstrap.level_id());
		if (!target)
			return {false, "Native world target is not registered."};
		if (!bootstrap.has_profile())
			return {false, "Server bootstrap has no authoritative profile."};

		auto *menu = guarded_find_main_menu();
		if (!menu || menu->m_saveLoad.m_flag72 != 0)
		{
			return {
			    false,
			    "The native main-menu New Game controller is not ready."};
		}

		{
			std::scoped_lock lock(m_cache_mutex);
			if (m_world_start_bootstrap)
				return {false, "A native world start is already in progress."};
			m_world_start_bootstrap = bootstrap;
			m_world_start_level_id = std::string(target->id);
			m_world_start_level_name = std::string(target->name);
			m_world_start_requires_lifecycle = true;
			m_sandbox_progress = {};
			m_sandbox_progress.phase = sandbox_phase::loading;
			m_world_start_stage = world_start_stage::invoking_new_game;
			m_diagnostic = "Invoking native New Game for level "
			    + m_world_start_level_name + ".";
		}
		m_expected_epoch_transition.store(true, std::memory_order_release);
		m_world_lifecycle_seen.store(false, std::memory_order_release);
		m_world_pre_data_seen.store(false, std::memory_order_release);
		KCD2Online_JOIN_TRACE(
		    "join.world.new-game.invoke",
		    std::format(
		        "level_id={} level_name={} menu={} save_load={} rel_id=141236",
		        target->id,
		        target->name,
		        static_cast<void *>(menu),
		        static_cast<void *>(&menu->m_saveLoad)));
		if (!guarded_start_debug_new_game(
		        &menu->m_saveLoad,
		        m_world_start_level_name.c_str()))
		{
			fail_native_world_start(
			    "The native Warhorse New Game entry rejected the request.");
			return {false, poll_sandbox().error};
		}
		set_world_start_stage(
		    world_start_stage::waiting_for_lifecycle,
		    "Native New Game accepted; waiting for lifecycle events.");
		return {true, {}};
	}

	void native_runtime::advance_native_world_start()
	{
		std::optional<protocol::ServerBootstrap> bootstrap;
		world_start_stage stage{};
		bool requires_lifecycle{};
		std::string target_id;
		{
			std::scoped_lock lock(m_cache_mutex);
			if (!m_world_start_bootstrap)
				return;
			bootstrap = m_world_start_bootstrap;
			stage = m_world_start_stage;
			requires_lifecycle = m_world_start_requires_lifecycle;
			target_id = m_world_start_level_id;
		}
		if (stage == world_start_stage::failed
		    || stage == world_start_stage::activating)
			return;
		if (requires_lifecycle
		    && !m_world_lifecycle_seen.load(std::memory_order_acquire))
		{
			set_world_start_stage(
			    world_start_stage::waiting_for_lifecycle,
			    "Waiting for native NewGame/PreDataLoaded lifecycle events.");
			return;
		}
		if (!m_data_loaded.load(std::memory_order_acquire))
		{
			set_world_start_stage(
			    world_start_stage::waiting_for_data,
			    "Native loading is active; waiting for DataLoaded.");
			return;
		}

		std::uint64_t capabilities{};
		std::string current_level;
		bool player_ready{};
		bool transition_safe{};
		bool level_load_complete{};
		bool probe_failed{};
		std::string probe_error;
		{
			std::scoped_lock lock(m_cache_mutex);
			capabilities = m_capabilities;
			current_level = m_level_id;
			player_ready = m_local_transform.has_value();
			transition_safe = m_transition_safe;
			level_load_complete = m_level_load_complete;
			probe_failed = m_probe_failed.load(std::memory_order_acquire);
			probe_error = m_probe_error;
		}
		if (current_level != target_id)
		{
			set_world_start_stage(
			    world_start_stage::waiting_for_level,
			    "DataLoaded fired; waiting for wh_sys_BaseLevelId="
			        + target_id + ".");
			return;
		}
		if (!level_load_complete)
		{
			set_world_start_stage(
			    world_start_stage::waiting_for_level,
			    "Target level is present; waiting for the engine to reach RUNNING after LEVEL_LOAD_COMPLETE.");
			return;
		}
		if (!player_ready || !transition_safe)
		{
			set_world_start_stage(
			    world_start_stage::waiting_for_player,
			    "Target level is loaded; waiting for native player modules.");
			return;
		}
		if (probe_failed)
		{
			fail_native_world_start(
			    probe_error.empty()
			        ? "Native multiplayer readiness probe failed after New Game."
			        : probe_error);
			return;
		}
		const auto missing =
		    required_client_runtime_capabilities & ~capabilities;
		if (missing != 0)
		{
			set_world_start_stage(
			    world_start_stage::probing_runtime,
			    std::format(
			        "Player is ready; validating native capabilities (missing 0x{:X}).",
			        missing));
			return;
		}

		set_world_start_stage(
		    world_start_stage::activating,
		    "Native world is ready; applying authoritative server state.");
		const auto result = activate_loaded_sandbox(*bootstrap);
		if (!result.started)
			fail_native_world_start(result.error);
	}

	void native_runtime::set_world_start_stage(
	    world_start_stage stage,
	    std::string diagnostic)
	{
		bool changed{};
		{
			std::scoped_lock lock(m_cache_mutex);
			if (!m_world_start_bootstrap || m_world_start_stage == stage)
				return;
			m_world_start_stage = stage;
			m_diagnostic = diagnostic;
			changed = true;
		}
		if (changed)
			KCD2Online_JOIN_TRACE("join.world.stage", diagnostic);
	}

	void native_runtime::fail_native_world_start(std::string error)
	{
		{
			std::scoped_lock lock(m_cache_mutex);
			m_world_start_stage = world_start_stage::failed;
			m_sandbox_progress.phase = sandbox_phase::failed;
			m_sandbox_progress.error = error;
			m_diagnostic = error;
		}
		m_expected_epoch_transition.store(false, std::memory_order_release);
		KCD2Online_JOIN_TRACE("join.world.failed", error);
	}

	sandbox_poll_result native_runtime::poll_sandbox()
	{
		std::scoped_lock lock(m_cache_mutex);
		return m_sandbox_progress;
	}

	bool native_runtime::sandbox_active() const
	{
		std::scoped_lock lock(m_cache_mutex);
		return m_sandbox_active;
	}

	void native_runtime::end_sandbox(std::string_view error)
	{
		remove_home_marker();
		m_home_marker.reset();
		m_pending_activity_start.reset();
		m_native_activity_kind = protocol::PLAYER_ACTIVITY_KIND_NONE;
		m_activity_end_pending = false;
		bool should_unload{};
		bool open_menu_only{};
		const auto world_loaded = !native_world_unloaded();
		{
			std::scoped_lock lock(m_cache_mutex);
			if (m_unload_pending)
			{
				if (!error.empty())
					m_sandbox_progress.error = std::string(error);
				return;
			}
			if (!m_sandbox_active && !m_world_start_bootstrap)
			{
				if (error.empty())
					return;
				if (!world_loaded)
				{
					m_main_menu_pending = true;
					open_menu_only = true;
				}
			}
			should_unload = !open_menu_only;
			if (should_unload)
			{
				m_sandbox_progress.phase = sandbox_phase::unloading;
				m_sandbox_progress.error = std::string(error);
			}
			else
			{
				m_sandbox_progress = {};
			}
			m_world_start_bootstrap.reset();
			m_world_start_level_id.clear();
			m_world_start_level_name.clear();
			m_world_start_requires_lifecycle = false;
			m_world_start_stage = world_start_stage::idle;
		}
		if (open_menu_only)
		{
			cancel_multiplayer_preparation();
			return;
		}
		if (should_unload)
			begin_native_unload(
			    error.empty()
			        ? "Native sandbox world unload is in progress."
			        : error);
	}

	std::string native_runtime::current_level_id() const
	{
		std::scoped_lock lock(m_cache_mutex);
		return m_level_id;
	}

	std::optional<protocol::PlayerProfile> native_runtime::local_profile()
	{
		std::string error;
		auto result = m_profiles.capture(error);
		if (!result)
		{
			std::scoped_lock lock(m_cache_mutex);
			m_diagnostic = error.empty()
			    ? "Native profile capture failed."
			    : std::move(error);
		}
		return result;
	}

	bool native_runtime::set_npc_entities_disabled(
	    bool humans_disabled,
	    bool animals_disabled)
	{
		std::string error;
		const auto result = m_entities.set_world_isolated(
		    humans_disabled,
		    animals_disabled,
		    error);
		if (!result)
		{
			std::scoped_lock lock(m_cache_mutex);
			m_diagnostic = std::move(error);
		}
		return result;
	}

	std::vector<protocol::WorldObjectState>
	native_runtime::poll_world_object_updates()
	{
		auto updates = m_entities.poll_world_object_updates();
		if (!updates.empty())
			(void)execute_console_command("cheat_own_stolen_items");
		return updates;
	}

	bool native_runtime::apply_world_object_state(
	    const protocol::WorldObjectState &state)
	{
		std::string error;
		const bool result = m_entities.apply_world_object_state(state, error);
		if (!result)
		{
			std::scoped_lock lock(m_cache_mutex);
			m_diagnostic = std::move(error);
		}
		return result;
	}

	std::vector<protocol::WorldItemState>
	native_runtime::poll_world_item_updates()
	{
		return m_entities.poll_world_item_updates();
	}

	bool native_runtime::apply_world_item_state(
	    const protocol::WorldItemState &state)
	{
		std::string error;
		const bool result = m_entities.apply_world_item_state(state, error);
		if (!result)
		{
			std::scoped_lock lock(m_cache_mutex);
			m_diagnostic = std::move(error);
		}
		return result;
	}

	std::vector<protocol::NpcObservation>
	native_runtime::poll_npc_observations()
	{
		return m_entities.poll_npc_observations();
	}

	bool native_runtime::apply_npc_state(
	    const protocol::NpcState &state,
	    bool local_authority)
	{
		std::string error;
		const bool result = m_entities.apply_npc_state(
		    state, local_authority, error);
		if (!result)
		{
			std::scoped_lock lock(m_cache_mutex);
			m_diagnostic = std::move(error);
		}
		return result;
	}

	void native_runtime::remove_npc_state(
	    std::uint64_t npc_id,
	    std::uint32_t generation)
	{
		m_entities.remove_npc_state(npc_id, generation);
	}

	bool native_runtime::apply_environment_state(
	    const protocol::EnvironmentState &state,
	    bool apply_weather)
	{
		if (!is_valid_environment_state(state))
		{
			std::scoped_lock lock(m_cache_mutex);
			m_diagnostic = "Native environment synchronization received an invalid state.";
			KCD2Online_JOIN_TRACE(
			    "join.environment.apply.failed",
			    m_diagnostic);
			return false;
		}
		std::string error;
		if (!force_set_environment_cvar(
		        "e_TimeOfDay",
		        state.time_of_day_hours(),
		        true,
		        error)
		    || !force_set_environment_cvar(
		        "e_TimeOfDaySpeed",
		        state.time_scale(),
		        false,
		        error))
		{
			std::scoped_lock lock(m_cache_mutex);
			m_diagnostic = "Native environment synchronization failed: " + error;
			KCD2Online_JOIN_TRACE(
			    "join.environment.apply.failed",
			    m_diagnostic);
			return false;
		}
		if (apply_weather)
		{
			const auto command = std::format(
			    "cheat_set_weather id:{} transition:{:.3f}",
			    state.weather_id(),
			    static_cast<double>(state.weather_transition_ms()) / 1000.0);
			if (!execute_console_command(command.c_str()))
			{
				std::scoped_lock lock(m_cache_mutex);
				m_diagnostic =
				    "Native environment synchronization could not apply weather.";
				KCD2Online_JOIN_TRACE(
				    "join.environment.apply.failed",
				    m_diagnostic);
				return false;
			}
		}
		KCD2Online_JOIN_TRACE(
		    "join.environment.apply.ok",
		    std::format(
		        "revision={} weather_revision={} apply_weather={}",
		        state.revision(),
		        state.weather_revision(),
		        apply_weather));
		return true;
	}

	bool native_runtime::set_home_marker(
	    const std::optional<protocol::PropertyHomeMarker> &marker)
	{
		if (marker
		    && (marker->property_id().empty() || marker->level_id().empty()
		        || !marker->has_position() || marker->entity_guid() == 0
		        || !std::isfinite(marker->position().x())
		        || !std::isfinite(marker->position().y())
		        || !std::isfinite(marker->position().z())))
			return false;
		if (m_home_marker && marker
		    && m_home_marker->SerializeAsString() == marker->SerializeAsString())
			return true;
		remove_home_marker();
		m_home_marker = marker;
		refresh_home_marker();
		return true;
	}

	void native_runtime::refresh_home_marker()
	{
		if (!m_home_marker || m_native_home_mark || !sandbox_active()
		    || canonical_level_id(current_level_id())
		        != canonical_level_id(m_home_marker->level_id()))
			return;
		const auto now = std::chrono::steady_clock::now();
		if (now < m_next_home_marker_attempt)
			return;
		// A property's anchor may legitimately not be streamed yet.  Retrying
		// once per frame turns that normal state into a full-time Lua/entity/AI
		// lookup and was the source of the severe frame-rate drop.
		m_next_home_marker_attempt = now + std::chrono::seconds{1};
		home_marker_apply_result result;
		if (!guarded_apply_home_marker(*m_home_marker, result))
			return;
		m_home_filter_was_visible = result.filter_was_visible;
		m_native_home_map = result.map;
		m_native_home_mark = std::move(result.mark);
		KCD2Online_JOIN_TRACE(
		    "property.home-marker.applied",
		    std::format(
		        "property_id=\"{}\" entity_guid={} entity_id={} name=\"{}\"",
		        m_home_marker->property_id(),
		        m_home_marker->entity_guid(),
		        result.entity_id,
		        m_home_marker->display_name()));
	}

	void native_runtime::remove_home_marker()
	{
		m_next_home_marker_attempt = {};
		if (!m_native_home_mark)
			return;
#ifdef _WIN32
		__try
		{
#endif
			auto *current = wh::guimodule::C_UIMap::GetInstance();
			if (current && current == m_native_home_map)
			{
				remove_home_entity_mark(current, m_native_home_mark);
				if (!m_home_filter_was_visible)
					current->SetPoiMarkersVisible(
					    wh::guimodule::E_MarkType::Home, false);
			}
#ifdef _WIN32
		}
		__except(KCD2Online_JOIN_SEH_FILTER("home-marker.remove.seh"))
		{
		}
#endif
		m_native_home_mark.reset();
		m_native_home_map = nullptr;
		m_home_filter_was_visible = false;
	}

	bool native_runtime::apply_authoritative_profile(
	    const protocol::PlayerProfile &profile)
	{
		if (!is_valid_profile(profile))
			return false;
		std::string capture_error;
		if (const auto current = m_profiles.capture(capture_error);
		    current && same_native_profile_state(*current, profile))
		{
			// Only the server revision/identity or sampled transform changed. The
			// inventory is already correct, so touching live trading/equipment
			// objects would add risk without changing native state.
			m_profiles.set_wire_identity(profile);
			return true;
		}
		const auto applied = reconcile_profile(m_profiles, profile);
		if (!applied.success)
		{
			std::scoped_lock lock(m_cache_mutex);
			m_diagnostic = "Native profile correction failed: " + applied.error;
			return false;
		}
		m_profiles.set_wire_identity(profile);
		(void)execute_console_command("cheat_own_stolen_items");
		return true;
	}

	bool native_runtime::respawn_local_player(
	    const protocol::TransformState &spawn)
	{
		if (!is_finite_transform(spawn))
			return false;
		auto fail_respawn = [this](std::string error)
		{
			KCD2Online_JOIN_TRACE("join.respawn.native.failed", error);
			std::scoped_lock lock(m_cache_mutex);
			m_diagnostic = std::move(error);
			return false;
		};

		const auto before_player = m_entities.player();
		const auto before_identity =
		    capture_player_respawn_identity(before_player);
		if (!before_identity.complete())
		{
			return fail_respawn(
			    "Native respawn requires an existing client Entity, Actor, and Soul.");
		}
		auto *framework = CCryAction::GetInstance();
		if (!framework || !before_player.actor->IsPlayer()
		    || framework->GetClientEntity() != before_player.entity
		    || framework->GetClientActor() != before_player.actor)
		{
			return fail_respawn(
			    "Native respawn rejected an inconsistent local-player ownership graph.");
		}
		KCD2Online_JOIN_TRACE(
		    "join.respawn.native.begin",
		    std::format(
		        "entity_id={} entity={} actor={} soul={} health={:.3f} "
		        "dead={} spawn=({:.3f},{:.3f},{:.3f})",
		        before_identity.entity_id,
		        reinterpret_cast<void *>(before_identity.entity),
		        reinterpret_cast<void *>(before_identity.actor),
		        reinterpret_cast<void *>(before_identity.soul),
		        before_player.actor->GetHealth(),
		        before_player.actor->IsDead(),
		        spawn.position().x(),
		        spawn.position().y(),
		        spawn.position().z()));

		// A death can interrupt a native minigame or another player-module-owned
		// activity. Release our local handle before reviving the same C_Player;
		// creating a replacement actor would orphan those global module links.
		cancel_local_activity();
		constexpr auto revive_result = "KCD2Online_RespawnRevived";
		clear_script_global(revive_result);
		const auto revive_dispatched = execute_script(
		    "KCD2Online_RespawnRevived=false; "
		    "if player and player.actor then "
		    "local actor=player.actor; "
		    "actor:Revive(false); "
		    "local maximum=actor:GetMaxHealth(); "
		    "if maximum and maximum>0 then actor:SetHealth(maximum) end; "
		    "KCD2Online_RespawnRevived=(actor:GetHealth()>0 "
		    "and not actor:IsDead() and not actor:IsUnconscious()); end");
		bool revived{};
		if (!revive_dispatched
		    || !take_script_global_bool(revive_result, revived) || !revived)
		{
			return fail_respawn(
			    "Native C_Player revive did not reach a living, conscious state.");
		}

		const auto revived_player = m_entities.player();
		const auto revived_identity =
		    capture_player_respawn_identity(revived_player);
		const auto revive_identity_result = validate_player_respawn_identity(
		    before_identity,
		    revived_identity);
		if (revive_identity_result != player_respawn_identity_result::stable)
		{
			return fail_respawn(std::format(
			    "Native revive violated player identity: {}.",
			    to_string(revive_identity_result)));
		}
		if (!apply_local_correction(spawn))
			return false;

		// SetWorldTM rotates the Entity, but the local first-person view has its
		// own actor state. Mirror Vanilla's initial-spawn PlayerSetViewAngles step
		// so the camera and body start with the same authoritative yaw.
		constexpr auto settled_result = "KCD2Online_RespawnSettled";
		clear_script_global(settled_result);
		const auto view_dispatched = execute_script(
		    "KCD2Online_RespawnSettled=false; "
		    "if player and player.actor then "
		    "local angles={x=0,y=0,z=0}; player:GetAngles(angles); "
		    "angles.x=0; angles.y=0; "
		    "player.actor:PlayerSetViewAngles(angles); "
		    "KCD2Online_RespawnSettled=(player.actor:GetHealth()>0 "
		    "and not player.actor:IsDead() "
		    "and not player.actor:IsUnconscious()); end");
		bool settled{};
		if (!view_dispatched
		    || !take_script_global_bool(settled_result, settled) || !settled)
		{
			return fail_respawn(
			    "Native player did not remain alive and conscious after spawn correction.");
		}

		const auto final_player = m_entities.player();
		const auto final_identity =
		    capture_player_respawn_identity(final_player);
		const auto final_identity_result = validate_player_respawn_identity(
		    before_identity,
		    final_identity);
		if (final_identity_result != player_respawn_identity_result::stable)
		{
			return fail_respawn(std::format(
			    "Native respawn violated player identity after correction: {}.",
			    to_string(final_identity_result)));
		}

		const auto game_over_hidden = guarded_hide_game_over();
		KCD2Online_JOIN_TRACE(
		    "join.respawn.native.complete",
		    std::format(
		        "entity_id={} actor={} soul={} health={:.3f} "
		        "identity={} game_over_hidden={}",
		        final_identity.entity_id,
		        reinterpret_cast<void *>(final_identity.actor),
		        reinterpret_cast<void *>(final_identity.soul),
		        final_player.actor->GetHealth(),
		        to_string(final_identity_result),
		        game_over_hidden));
		if (!game_over_hidden)
		{
			std::scoped_lock lock(m_cache_mutex);
			m_diagnostic =
			    "Native player revived, but the game-over fader could not be hidden.";
		}
		return true;
	}

	bool native_runtime::local_player_dead() const
	{
		const auto *actor = m_entities.player().actor;
		return actor && actor->m_health <= 0.0F;
	}

	bool native_runtime::local_player_laying() const
	{
		constexpr std::string_view query =
		    "KCD2Online_PlayerLaying = player and player.IsLaying "
		    "and player:IsLaying() or false";
		clear_script_global("KCD2Online_PlayerLaying");
		if (!execute_script(query))
			return false;
		bool result{};
		return take_script_global_bool("KCD2Online_PlayerLaying", result)
		    && result;
	}

	void native_runtime::show_multiplayer_notice(
	    std::string_view message)
	{
		const auto script = "if Game and Game.SendInfoText then "
		    "Game.SendInfoText(" + lua_string(message) + ", false) end";
		(void)execute_script(script);
	}

	std::optional<protocol::TransformState>
	native_runtime::local_transform() const
	{
		std::scoped_lock lock(m_cache_mutex);
		return m_local_transform;
	}

	std::optional<protocol::AvatarDescriptor>
	native_runtime::local_avatar_visual() const
	{
		std::string error;
		return m_profiles.capture_avatar_visual(error);
	}

	bool native_runtime::apply_local_correction(
	    const protocol::TransformState &transform)
	{
		std::string error;
		if (!m_entities.write_transform(m_entities.player().entity, transform, error))
		{
			std::scoped_lock lock(m_cache_mutex);
			m_diagnostic = std::move(error);
			return false;
		}
		std::scoped_lock lock(m_cache_mutex);
		m_local_transform            = transform;
		m_local_transform_sampled_at = std::chrono::steady_clock::now();
		m_transform_sequence         = 0;
		return true;
	}

	remote_avatar_sync_result native_runtime::sync_remote_players(std::span<const remote_avatar_snapshot> players)
	{
		KCD2Online_JOIN_TRACE("join.remote-sync.begin", std::format("players={} epoch={}", players.size(), m_epoch.load(std::memory_order_acquire)));
		m_remote_backend.set_epoch(
		    m_epoch.load(std::memory_order_acquire));
		auto result = m_remote_avatars.sync(players);
		KCD2Online_JOIN_TRACE(
		    result.success ? "join.remote-sync.complete"
		                   : "join.remote-sync.failed",
		    std::format(
		        "success={} degraded={} spawned={} updated={} removed={} "
		        "error=\"{}\" diagnostic=\"{}\"",
		        result.success,
		        result.degraded,
		        result.spawned,
		        result.updated,
		        result.removed,
		        result.error,
		        result.diagnostic));
		return result;
	}

	std::uint64_t native_runtime::epoch() const noexcept
	{
		return m_epoch.load(std::memory_order_acquire);
	}

	void native_runtime::invalidate_epoch_on_game_thread()
	{
		remove_home_marker();
		m_pending_activity_start.reset();
		m_native_activity_kind = protocol::PLAYER_ACTIVITY_KIND_NONE;
		m_activity_end_pending = false;
		const auto expected_transition =
		    m_expected_epoch_transition.load(std::memory_order_acquire);
		if (!expected_transition)
			m_multiplayer_requested.store(false, std::memory_order_release);
		m_remote_avatars.clear();
		m_remote_backend.clear();
		m_entities.restore_world();
		m_profiles.reset();
		m_remote_backend.reset_active_probe();
		if (!m_unload_pending && !expected_transition)
		{
			restore_save_load();
		}
		m_epoch.fetch_add(1, std::memory_order_acq_rel);
		std::scoped_lock lock(m_cache_mutex);
		m_level_id.clear();
		m_local_transform.reset();
		m_local_transform_sampled_at = {};
		m_transform_sequence         = 0;
		m_local_animation_fragment.clear();
		m_animation_sequence         = 0;
		m_animation_started_at_ms    = 0;
		m_probe_transform_verified   = false;
		m_probe_complete             = false;
		m_probe_failed               = false;
		m_preparation_active         = false;
		m_preparation_frames         = 0;
		m_probe_error.clear();
		if (!m_unload_pending && !expected_transition)
		{
			m_sandbox_active   = false;
			m_sandbox_progress = {};
		}
		if (expected_transition)
		{
			m_sandbox_progress.phase = sandbox_phase::loading;
			m_diagnostic =
			    "Expected native world transition invalidated the old runtime epoch.";
			KCD2Online_JOIN_TRACE(
			    "join.world.epoch.expected",
			    std::format(
			        "new_epoch={} target_level={}",
			        m_epoch.load(std::memory_order_acquire),
			        m_world_start_level_id));
		}
	}

	void native_runtime::refresh_cached_state()
	{
		const auto multiplayer_requested =
		    m_multiplayer_requested.load(std::memory_order_acquire);
		std::uint64_t capabilities =
		    runtime_capability_kcse | runtime_capability_game_thread;
		std::string level;
		std::optional<protocol::TransformState> transform;
		std::string sampled_animation_fragment;
		bool transition_safe = false;
		std::string transition_blocker;
		std::uint32_t system_global_state{};
		const auto system_state_available =
		    read_system_global_state(system_global_state);
		const auto level_load_complete = system_state_available
		    && system_global_state == system_global_state_running;

		auto *framework = CCryAction::GetInstance();
		const auto native_player = m_entities.player();
		auto *entity = native_player.entity;
		auto *framework_actor =
		    framework ? framework->GetClientActor() : nullptr;
		auto *context_actor = native_player.actor;
		auto *context = wh::game::S_GameContext::GetInstance();
		auto *player_module = context ? context->m_pPlayerModule : nullptr;
		if (!level_load_complete)
		{
			transition_blocker = system_state_available
			    ? std::format(
			          "Native level loading is still active (system state {}).",
			          system_global_state)
			    : "Native level-loading state is not available yet.";
		}
		else if (player_module)
		{
			transition_safe = true;
			if (player_module->m_pFastTravel
			    && player_module->m_pFastTravel->IsFastTraveling())
			{
				transition_safe = false;
				transition_blocker =
				    "Finish or cancel fast travel before connecting.";
			}
			else if (player_module->m_pMinigameManager)
			{
				for (const auto &[user_id, session] :
				     player_module->m_pMinigameManager->m_sessions)
				{
					(void)user_id;
					if (session && !session->IsFinished())
					{
						transition_safe = false;
						transition_blocker =
						    "Exit the active minigame before connecting.";
						break;
					}
				}
			}
		}
		else
		{
			transition_blocker =
			    "The native PlayerModule is not ready for multiplayer.";
		}
		if (environment_runtime_available())
			capabilities |= runtime_capability_environment;
		KCD2Online_JOIN_TRACE(
		    "join.runtime.cached-state.precheck",
		    std::format(
		        "multiplayer_requested={} framework={} entity={} "
		        "framework_actor={} context_actor={} system_state={} "
		        "level_load_complete={}",
		        multiplayer_requested,
		        static_cast<void *>(framework),
		        static_cast<void *>(entity),
		        static_cast<void *>(framework_actor),
		        static_cast<void *>(context_actor),
		        system_state_available
		            ? std::to_string(system_global_state)
		            : "unavailable",
		        level_load_complete));
		if (entity && framework_actor && context_actor)
		{
			capabilities |= runtime_capability_local_player;
			if (auto *matrix = entity->GetWorldTMPtr())
			{
				protocol::TransformState state;
				state.mutable_position()->set_x(matrix->m03);
				state.mutable_position()->set_y(matrix->m13);
				state.mutable_position()->set_z(matrix->m23);
				*state.mutable_rotation() = quaternion_from_matrix(*matrix);
				state.mutable_velocity();
				state.set_client_time_ms(now_ms());
				transform = std::move(state);
				capabilities |= runtime_capability_transform_read;
			}
			const auto *fragment = context_actor->m_fragmentName.c_str();
			if (fragment && replayable_non_combat_fragment(fragment))
				sampled_animation_fragment = fragment;
		}

		std::string profile_error;
		bool profile_ready{};
		bool avatar_ready{};
		if (multiplayer_requested && level_load_complete
		    && (capabilities & runtime_capability_local_player) != 0)
		{
			if (m_probe_complete)
			{
				// The native readiness chain is scoped to the current runtime
				// epoch. Lifecycle invalidation resets the completed probe, so
				// repeating inventory/catalog validation every frame adds cost
				// without providing additional safety.
				profile_ready = true;
				avatar_ready = true;
			}
			else if (!m_probe_failed.load(std::memory_order_acquire))
			{
				profile_ready = m_profiles.ready(profile_error);
				avatar_ready = m_remote_backend.available();
			}
		}
		KCD2Online_JOIN_TRACE(
		    "join.runtime.capability-components",
		    std::format(
		        "transform_ready={} profile_ready={} avatar_ready={} "
		        "probe_started={} probe_transform_verified={} "
		        "probe_complete={} probe_failed={} frame={}",
		        transform.has_value(),
		        profile_ready,
		        avatar_ready,
		        m_preparation_active,
		        m_probe_transform_verified,
		        m_probe_complete,
		        m_probe_failed.load(std::memory_order_acquire),
		        m_preparation_frames));
		if (multiplayer_requested && level_load_complete
		    && (capabilities & runtime_capability_local_player) != 0
		    && !m_preparation_active)
		{
			m_remote_backend.reset_active_probe();
			m_probe_transform_verified = false;
			m_probe_complete = false;
			m_probe_failed = false;
			m_probe_error.clear();
			m_preparation_frames = 0;
			m_preparation_active = true;
		}
		if (multiplayer_requested && level_load_complete && transform
		    && profile_ready && avatar_ready
		    && !m_probe_complete && !m_probe_failed)
		{
			if (!m_probe_transform_verified)
			{
				std::string probe_error;
				if (!m_entities.write_transform(
				        entity,
				        *transform,
				        probe_error))
				{
					m_probe_failed = true;
					m_probe_error =
					    "active identical SetWorldTM probe failed: "
					    + probe_error;
					KCD2Online_JOIN_TRACE(
					    "join.runtime.probe.identical-transform.failed",
					    m_probe_error);
				}
				else
				{
					m_probe_transform_verified = true;
					m_transform_sequence = 0;
					KCD2Online_JOIN_TRACE(
					    "join.runtime.probe.identical-transform.ok",
					    "SetWorldTM write/readback verified");
				}
			}
			if (m_probe_transform_verified && !m_probe_failed)
			{
				std::string probe_error;
				switch (m_remote_backend.poll_active_probe(
				    *transform,
				    probe_error))
				{
				case native_remote_avatar_backend::active_probe_result::
				    succeeded:
					m_probe_complete = true;
					KCD2Online_JOIN_TRACE(
					    "join.runtime.probe.remote-avatar.ok",
					    "Actor/Soul/Inventory/Equipment probe succeeded");
					break;
				case native_remote_avatar_backend::active_probe_result::
				    failed:
					m_probe_failed = true;
					m_probe_error =
					    "active native Actor/Soul/Inventory/Equipment "
					    "probe failed: "
					    + probe_error;
					KCD2Online_JOIN_TRACE(
					    "join.runtime.probe.remote-avatar.failed",
					    m_probe_error);
					break;
				case native_remote_avatar_backend::active_probe_result::
				    pending:
					break;
				}
			}
		}
		if (multiplayer_requested && m_preparation_active
		    && !m_probe_complete && !m_probe_failed
		    && ++m_preparation_frames > 900)
		{
			m_probe_failed = true;
			if (!transform)
				m_probe_error =
				    "Native local-player transform was not ready in time.";
			else if (!profile_ready)
				m_probe_error = profile_error.empty()
				    ? "Native player profile was not ready in time."
				    : std::move(profile_error);
			else if (!avatar_ready)
				m_probe_error = m_remote_backend.diagnostic();
			else
				m_probe_error =
				    "Native multiplayer capability probe timed out.";
		}
		if (multiplayer_requested && m_probe_complete)
		{
			capabilities |= runtime_capability_transform_write
			    | runtime_capability_sandbox
			    | runtime_capability_entity_isolation
			    | runtime_capability_equipment
			    | runtime_capability_profile_capture
			    | runtime_capability_profile_apply
			    | runtime_capability_profile_qam
			    | runtime_capability_remote_avatar
			    | runtime_capability_npc_sync;
		}
		if (player_module)
			capabilities |= runtime_capability_transition_gate;
		if (!m_address_library.empty()
		    && !m_address_library_distribution.empty()
		    && m_address_library_format != 0
		    && m_address_library_entries != 0
		    && m_address_library_sha256.size() == 64)
		{
			capabilities |= runtime_capability_address_library_identity;
		}

		auto *environment = SSystemGlobalEnvironment::GetInstance();
		KCD2Online_JOIN_TRACE(
		    "join.runtime.entity-system.state",
		    std::format(
		        "environment={} entity_system={} console={} local_entity={}",
		        static_cast<void *>(environment),
		        environment
		            ? static_cast<void *>(environment->pEntitySystem)
		            : nullptr,
		        environment
		            ? static_cast<void *>(environment->pConsole)
		            : nullptr,
		        static_cast<void *>(entity)));
		if (entity && environment && environment->pConsole)
		{
			if (auto *level_cvar = environment->pConsole->GetCVar("wh_sys_BaseLevelId"))
			{
				if (const auto *value = level_cvar->GetString())
				{
					level = value;
				}
			}
		}

		const auto transform_sampled_at = std::chrono::steady_clock::now();
		std::scoped_lock lock(m_cache_mutex);
		if (transform)
		{
			// IEntity only exposes the world transform here. Derive the wire
			// velocity from consecutive game-thread samples so the server can
			// classify locomotion instead of publishing every player as idle.
			if (m_local_transform && m_local_transform_sampled_at != std::chrono::steady_clock::time_point{})
			{
				const auto elapsed = std::chrono::duration<float>(transform_sampled_at - m_local_transform_sampled_at).count();
				if (elapsed >= 0.001F && elapsed <= 0.25F)
				{
					const auto &previous = m_local_transform->position();
					const auto &current  = transform->position();
					auto *velocity       = transform->mutable_velocity();
					velocity->set_x((current.x() - previous.x()) / elapsed);
					velocity->set_y((current.y() - previous.y()) / elapsed);
					velocity->set_z((current.z() - previous.z()) / elapsed);

					auto *locomotion = transform->mutable_locomotion();
					const auto &rotation = transform->rotation();
					const auto facing = facing_from_rotation(rotation);
					*locomotion->mutable_facing_direction() = facing;
					const auto speed = std::hypot(velocity->x(), velocity->y());
					locomotion->set_speed(speed);
					const auto &old_velocity = m_local_transform->velocity();
					auto *acceleration = locomotion->mutable_acceleration();
					acceleration->set_x((velocity->x() - old_velocity.x()) / elapsed);
					acceleration->set_y((velocity->y() - old_velocity.y()) / elapsed);
					acceleration->set_z((velocity->z() - old_velocity.z()) / elapsed);

					// Rotate world velocity into the actor's local frame. KCD2/Cry
					// uses +Y as actor forward.
					auto *local = locomotion->mutable_local_velocity();
					const auto &q = rotation;
					const auto right_x = 1.0F - 2.0F * (q.y() * q.y() + q.z() * q.z());
					const auto right_y = 2.0F * (q.x() * q.y() + q.w() * q.z());
					const auto right_z = 2.0F * (q.x() * q.z() - q.w() * q.y());
					local->set_x(velocity->x() * right_x + velocity->y() * right_y + velocity->z() * right_z);
					local->set_y(velocity->x() * facing.x() + velocity->y() * facing.y() + velocity->z() * facing.z());
					locomotion->set_strafing(
					    std::abs(local->x()) > 0.2F
					    && std::abs(local->y()) > 0.2F);

					const auto old_facing = facing_from_rotation(
					    m_local_transform->rotation());
					const auto yaw = std::atan2(facing.x(), facing.y());
					const auto old_yaw = std::atan2(old_facing.x(), old_facing.y());
					auto yaw_delta = std::remainder(
					    yaw - old_yaw,
					    2.0F * std::numbers::pi_v<float>);
					locomotion->set_yaw_rate(yaw_delta / elapsed);
				}
			}
			if (!transform->has_locomotion())
			{
				auto *locomotion = transform->mutable_locomotion();
				*locomotion->mutable_facing_direction() =
				    facing_from_rotation(transform->rotation());
				locomotion->mutable_local_velocity();
				locomotion->mutable_acceleration();
			}

			if (sampled_animation_fragment != m_local_animation_fragment)
			{
				m_local_animation_fragment = sampled_animation_fragment;
				++m_animation_sequence;
				m_animation_started_at_ms = now_ms();
			}
			if (m_animation_sequence != 0)
			{
				auto *animation = transform->mutable_animation();
				animation->set_sequence(m_animation_sequence);
				animation->set_fragment(m_local_animation_fragment);
				animation->set_started_at_ms(m_animation_started_at_ms);
				animation->set_active(!m_local_animation_fragment.empty());
			}
			transform->set_sequence(++m_transform_sequence);
			m_local_transform_sampled_at = transform_sampled_at;
		}
		else
		{
			m_local_transform_sampled_at = {};
		}
		m_local_transform    = std::move(transform);
		m_level_id           = std::move(level);
		m_capabilities       = capabilities;
		m_transition_safe    = transition_safe;
		m_transition_blocker = std::move(transition_blocker);
		m_level_load_complete = level_load_complete;
		KCD2Online_JOIN_TRACE("join.runtime.capabilities.updated",
		                  std::format("capabilities=0x{:X} required=0x{:X} missing=0x{:X} "
		                              "level=\"{}\"",
		                              capabilities,
		                              required_client_runtime_capabilities,
		                              required_client_runtime_capabilities & ~capabilities,
		                              m_level_id));
		if ((capabilities & runtime_capability_local_player) == 0)
		{
			if (!m_world_start_bootstrap)
				m_diagnostic =
				    "No native player is loaded; the server bootstrap can start "
				    "a save-free world.";
		}
		else if (!multiplayer_requested)
		{
			m_diagnostic =
			    "Multiplayer runtime is idle; click Connect to initialize it.";
		}
		else if ((capabilities & runtime_capability_transform_read) == 0)
		{
			m_diagnostic =
			    "The native player transform is not readable through the "
			    "verified libKCD2 vtable.";
		}
		else if (m_probe_failed)
		{
			m_diagnostic = m_probe_error;
		}
		else if (!m_probe_complete)
		{
			m_diagnostic =
			    "Active native multiplayer ABI probe is running.";
		}
		else
		{
			m_diagnostic =
			    profile_error.empty()
			        ? m_remote_backend.diagnostic()
			        : std::move(profile_error);
			if ((capabilities & runtime_capability_remote_avatar) != 0)
				m_diagnostic =
				    "All native multiplayer runtime capabilities are ready.";
		}
	}

	void native_runtime::restore_save_load()
	{
		if (!m_save_load_locked)
			return;
		if (auto *framework = CCryAction::GetInstance())
		{
			framework->AllowSave(true);
			framework->AllowLoad(true);
		}
		m_save_load_locked = false;
	}

	void native_runtime::begin_native_unload(std::string_view reason)
	{
		auto *framework = CCryAction::GetInstance();
		KCD2Online_JOIN_TRACE(
		    "join.sandbox.unload.begin",
		    std::format(
		        "reason=\"{}\" framework={} game_context={}",
		        reason,
		        static_cast<void *>(framework),
		        framework
		            ? static_cast<void *>(framework->m_pGameContext)
		            : nullptr));
		{
			std::scoped_lock lock(m_cache_mutex);
			m_unload_pending = true;
			m_unload_teardown_started = false;
			m_unload_command_queued = false;
			m_unload_deferred_logged = false;
			m_sandbox_active = true;
			m_sandbox_progress.phase = sandbox_phase::unloading;
			m_sandbox_progress.error = std::string(reason);
		}
		m_multiplayer_requested.store(false, std::memory_order_release);
		m_expected_epoch_transition.store(false, std::memory_order_release);
		queue_native_unload_if_safe();
		finish_native_unload_if_complete();
	}

	void native_runtime::queue_native_unload_if_safe()
	{
		bool teardown_started{};
		bool command_queued{};
		bool level_load_complete{};
		{
			std::scoped_lock lock(m_cache_mutex);
			if (!m_unload_pending)
				return;
			teardown_started = m_unload_teardown_started;
			command_queued = m_unload_command_queued;
			level_load_complete = m_level_load_complete;
		}
		if (native_world_unloaded())
			return;
		if (command_queued)
			return;
		if (!level_load_complete)
		{
			bool should_log{};
			{
				std::scoped_lock lock(m_cache_mutex);
				if (!m_unload_deferred_logged)
				{
					m_unload_deferred_logged = true;
					should_log = true;
				}
			}
			if (should_log)
			{
				KCD2Online_JOIN_TRACE(
				    "join.sandbox.unload.deferred",
				    "Native unload is waiting for the engine to reach RUNNING after LEVEL_LOAD_COMPLETE.");
			}
			return;
		}

		if (!teardown_started)
		{
			{
				std::scoped_lock lock(m_cache_mutex);
				if (m_unload_teardown_started)
					teardown_started = true;
				else
					m_unload_teardown_started = true;
			}
			if (!teardown_started)
			{
				(void)execute_script(
				    "if player and player.EnableFastTravel then "
				    "player:EnableFastTravel(true) end");
				m_remote_avatars.clear();
				m_remote_backend.clear();
				m_entities.restore_world();
				m_profiles.reset();
			}
		}

		auto *framework = CCryAction::GetInstance();
		if (framework && framework->m_pGameContext)
		{
			// EndGameContext is unsafe from KCSE's PostUpdate callback. Queue the
			// engine's map-unload command so CryEngine performs the transition in
			// its deferred console-command phase on the following frame.
			KCD2Online_JOIN_TRACE(
			    "join.sandbox.unload.command.begin",
			    std::format(
			        "framework={} game_context={}",
			        static_cast<void *>(framework),
			        static_cast<void *>(framework->m_pGameContext)));
			const auto queued = execute_console_command("unload", true);
			if (queued)
			{
				std::scoped_lock lock(m_cache_mutex);
				m_unload_command_queued = true;
			}
			KCD2Online_JOIN_TRACE(
			    queued
			        ? "join.sandbox.unload.command.queued"
			        : "join.sandbox.unload.command.failed",
			    queued
			        ? "Deferred native map unload was queued."
			        : "Deferred native map unload could not be queued.");
		}
		else
		{
			KCD2Online_JOIN_TRACE(
			    "join.sandbox.unload.command.skipped",
			    "framework or game context is nil");
		}
	}

	bool native_runtime::native_world_unloaded() const
	{
		auto *framework = CCryAction::GetInstance();
		return !framework
		    || (!framework->m_pGameContext
		        && framework->GetClientEntity() == nullptr);
	}

	void native_runtime::finish_native_unload_if_complete()
	{
		{
			std::scoped_lock lock(m_cache_mutex);
			if (!m_unload_pending)
				return;
		}
		queue_native_unload_if_safe();
		if (!native_world_unloaded())
			return;
		restore_save_load();
		{
			std::scoped_lock lock(m_cache_mutex);
			m_unload_pending = false;
			m_unload_teardown_started = false;
			m_unload_command_queued = false;
			m_unload_deferred_logged = false;
			m_sandbox_active = false;
			m_sandbox_progress = {};
			m_main_menu_pending = true;
		}
		KCD2Online_JOIN_TRACE(
		    "join.sandbox.unload.complete",
		    "Native world is unloaded; returning to the main menu.");
	}

	void native_runtime::open_main_menu_if_pending()
	{
		{
			std::scoped_lock lock(m_cache_mutex);
			if (!m_main_menu_pending)
				return;
		}
#ifdef _WIN32
		const auto opened = guarded_open_main_menu();
#else
		const auto opened = open_main_menu();
#endif
		if (!opened)
			return;
		{
			std::scoped_lock lock(m_cache_mutex);
			m_main_menu_pending = false;
		}
		KCD2Online_JOIN_TRACE(
		    "join.sandbox.main-menu.opened",
		    "Native root main menu is open.");
	}
}
