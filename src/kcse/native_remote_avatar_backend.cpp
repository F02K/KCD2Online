#include "kcse/native_remote_avatar_backend.hpp"
#include "kcse/native_avatar_combat.hpp"
#include "kcse/native_inventory.hpp"
#include "kcse/native_remote_avatar_equipment.hpp"
#include "kcse/join_trace.hpp"
#include "multiplayer/avatar_visual.hpp"
#include "multiplayer/emote_catalog.hpp"

#include "npc/equipment_catalog.hpp"

#include <entitymodule/C_Actor.h>
#include <entitymodule/C_Human.h>
#include <entitymodule/C_Inventory.h>
#include <entitymodule/C_Item.h>
#include <entitymodule/C_ItemDatabase.h>
#include <framework/GuidUtils.h>
#include <game/S_GameContext.h>
#include <rpgmodule/C_Soul.h>
#include <crysystem/SSystemGlobalEnvironment.h>
#include <crysystem/ScriptAnyValue.h>
#include <Offsets/vtables/IEntity.h>
#include <Offsets/vtables/IEntitySystem.h>
#include <Offsets/vtables/IActor.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <ranges>

namespace kcd2o::kcse
{
	namespace
	{
		// Soul.GetNameStringId() reads the CryString stored at C_Soul+0x3E8.
		// This is the name consumed by the target/interaction HUD; IEntity::GetName()
		// is only the engine's technical entity identifier.
		constexpr std::uintptr_t soul_display_name_string_id_offset = 0x3E8;
		constexpr auto remote_motion_retry =
		    std::chrono::milliseconds(500);
		constexpr auto remote_avatar_spawn_timeout =
		    std::chrono::seconds(10);
		constexpr auto remote_native_validation_interval =
		    std::chrono::seconds(1);
		std::chrono::steady_clock::time_point
		    last_slow_status_diagnostic_at{};
		std::chrono::steady_clock::time_point
		    last_update_diagnostic_at{};
		constexpr std::string_view probe_equipment_definition_id =
		    "c164f346-0463-4116-b790-094b11274e5e";

		bool position_or_rotation_changed(
		    const protocol::TransformState &left,
		    const protocol::TransformState &right)
		{
			constexpr float position_epsilon_squared = 0.000001F;
			constexpr float rotation_epsilon = 0.00001F;
			const auto dx = left.position().x() - right.position().x();
			const auto dy = left.position().y() - right.position().y();
			const auto dz = left.position().z() - right.position().z();
			if (dx * dx + dy * dy + dz * dz
			    > position_epsilon_squared)
			{
				return true;
			}

			const auto &a = left.rotation();
			const auto &b = right.rotation();
			const auto direct = std::abs(a.x() - b.x())
			    + std::abs(a.y() - b.y())
			    + std::abs(a.z() - b.z())
			    + std::abs(a.w() - b.w());
			const auto negated = std::abs(a.x() + b.x())
			    + std::abs(a.y() + b.y())
			    + std::abs(a.z() + b.z())
			    + std::abs(a.w() + b.w());
			return std::min(direct, negated) > rotation_epsilon;
		}

		Vec3 native_position(const protocol::Vec3 &position)
		{
			return Vec3(position.x(), position.y(), position.z());
		}

		wh::entitymodule::C_Actor *resolve_actor(
		    std::uint32_t entity_id)
		{
			auto *context = wh::game::S_GameContext::GetInstance();
			return context ? context->GetActorById(entity_id) : nullptr;
		}

		Offsets::IEntity *resolve_entity(std::uint32_t entity_id)
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			return environment && environment->pEntitySystem
			    ? environment->pEntitySystem->GetEntity(entity_id)
			    : nullptr;
		}

		enum class weapon_action_result
		{
			applied,
			rejected,
			faulted
		};

		weapon_action_result guarded_apply_weapon_state(
		    wh::entitymodule::C_Human &human,
		    const protocol::AvatarDescriptor &avatar) noexcept
		{
#ifdef _WIN32
			__try
			{
				return apply_native_avatar_weapon_state(human, avatar)
				    ? weapon_action_result::applied
				    : weapon_action_result::rejected;
			}
			__except(KCD2Online_JOIN_SEH_FILTER(
			    "join.remote-animation.weapon-action.seh"))
			{
				return weapon_action_result::faulted;
			}
#else
			return apply_native_avatar_weapon_state(human, avatar)
			    ? weapon_action_result::applied
			    : weapon_action_result::rejected;
#endif
		}

		bool execute_remote_script(std::string_view script) noexcept
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
				    "KCD2Online remote player activity",
				    nullptr);
#ifdef _WIN32
			}
			__except(KCD2Online_JOIN_SEH_FILTER(
			    "remote-player-script.seh"))
			{
				return false;
			}
#endif
		}

		std::string lua_string(std::string_view value)
		{
			std::string result{"\""};
			result.reserve(value.size() + 2);
			for (const char character : value)
			{
				if (character == '\\' || character == '\"')
					result.push_back('\\');
				result.push_back(character);
			}
			result.push_back('\"');
			return result;
		}

		bool remote_semantics_api_ready() noexcept
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			if (!environment || !environment->pScriptSystem)
				return false;
#ifdef _WIN32
			__try
			{
#endif
				auto function = environment->pScriptSystem
				    ->GetFunctionPtrTableName(
				        "Contexts", "SetPersistentOption");
				if (!function)
					return false;
				environment->pScriptSystem->ReleaseFunc(function);
				return true;
#ifdef _WIN32
			}
			__except(KCD2Online_JOIN_SEH_FILTER(
			    "remote-player-semantics-readiness.seh"))
			{
				return false;
			}
#endif
		}

		std::optional<int> take_remote_semantics_result() noexcept
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			if (!environment || !environment->pScriptSystem)
				return std::nullopt;
#ifdef _WIN32
			__try
			{
#endif
				ScriptAnyValue value;
				int result{};
				const bool read = environment->pScriptSystem->GetGlobalAny(
				    "KCD2Online_RemoteAvatarPolicyResult", value);
				if (read)
					(void)value.CopyTo(result);
				environment->pScriptSystem->SetGlobalToNull(
				    "KCD2Online_RemoteAvatarPolicyResult");
				return read ? std::optional<int>{result} : std::nullopt;
#ifdef _WIN32
			}
			__except(KCD2Online_JOIN_SEH_FILTER(
			    "remote-player-semantics-result.seh"))
			{
				return std::nullopt;
			}
#endif
		}

		std::optional<int> take_remote_script_result(
		    const char *global_name) noexcept
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			if (!environment || !environment->pScriptSystem || !global_name)
				return std::nullopt;
#ifdef _WIN32
			__try
			{
				ScriptAnyValue value;
				int result{};
				const bool read = environment->pScriptSystem->GetGlobalAny(
				    global_name, value);
				if (read)
					(void)value.CopyTo(result);
				environment->pScriptSystem->SetGlobalToNull(global_name);
				return read ? std::optional<int>{result} : std::nullopt;
			}
			__except(KCD2Online_JOIN_SEH_FILTER(
			    "remote-player-script-result.seh"))
			{
				return std::nullopt;
			}
#else
			ScriptAnyValue value;
			int result{};
			const bool read = environment->pScriptSystem->GetGlobalAny(
			    global_name, value);
			if (read)
				(void)value.CopyTo(result);
			environment->pScriptSystem->SetGlobalToNull(global_name);
			return read ? std::optional<int>{result} : std::nullopt;
#endif
		}

		bool queue_avatar_spawn(
		    std::string_view name,
		    std::string_view class_name,
		    std::string_view shared_soul_guid,
		    const Vec3 &position) noexcept
		{
			constexpr auto result_name =
			    "KCD2Online_RemoteAvatarSpawnResult";
			const auto script = std::format(
			    "{}=0 local ok,code=pcall(function() "
			    "KCD2Online_AvatarSpawnRequests="
			    "KCD2Online_AvatarSpawnRequests or {{}} "
			    "KCD2Online_AvatarSpawnRequests[{}]=true "
			    "Script.SetTimer(1,function() "
			    "local requests=KCD2Online_AvatarSpawnRequests "
			    "if not requests or not requests[{}] then return end "
			    "requests[{}]=nil "
				    "System.LogAlways('[KCD2Online] Avatar spawn begin '..{}) "
				    "local entity=nil local backend='none' local spawnError='none' "
				    "if XGenAIModule and type(XGenAIModule.SpawnEntity)=='function' then "
				    "backend='xgen' local spawned,xgenError=pcall(function() "
				    "XGenAIModule.SpawnEntity{{Name={},ClassName={},"
				    "Pos={{{:.9g},{:.9g},{:.9g}}},SharedSoulGuid={}}} end) "
				    "if spawned then entity=System.GetEntityByName({}) "
				    "else spawnError=xgenError end end "
				    "if not entity and System and type(System.SpawnEntity)=='function' then "
				    "backend='system' local spawned,systemResult=pcall(System.SpawnEntity,{{"
				    "class={},position={{x={:.9g},y={:.9g},z={:.9g}}},name={},"
				    "properties={{esFaction='Civilians',guidSharedSoulId={}}}}}) "
				    "if spawned then entity=System.GetEntityByName({}) "
				    "if not entity and type(systemResult)=='table' then entity=systemResult end "
				    "else spawnError=systemResult end end "
				    "local entityId=entity and entity.id or 0 "
				    "System.LogAlways('[KCD2Online] Avatar spawn end '..{}.."
				    "' backend='..backend..' ok='..tostring(entityId~=0).."
				    "' entity_id='..tostring(entityId)..' error='..tostring(spawnError)) end) "
				    "return 1 end) {}=ok and code or -1",
			    result_name,
			    lua_string(name),
			    lua_string(name),
			    lua_string(name),
			    lua_string(name),
			    lua_string(name),
			    lua_string(class_name),
			    position.x,
			    position.y,
				    position.z,
				    lua_string(shared_soul_guid),
				    lua_string(name),
				    lua_string(class_name),
				    position.x,
				    position.y,
				    position.z,
				    lua_string(name),
				    lua_string(shared_soul_guid),
				    lua_string(name),
				    lua_string(name),
				    result_name);
			if (!execute_remote_script(script))
				return false;
			const auto result = take_remote_script_result(result_name);
			return result && *result == 1;
		}

		void cancel_avatar_spawn(std::string_view name) noexcept
		{
			if (name.empty())
				return;
			(void)execute_remote_script(std::format(
			    "if KCD2Online_AvatarSpawnRequests then "
			    "KCD2Online_AvatarSpawnRequests[{}]=nil end",
			    lua_string(name)));
		}

		void queue_avatar_remove(std::string_view name) noexcept
		{
			if (name.empty())
				return;
			(void)execute_remote_script(std::format(
			    "Script.SetTimer(1,function() local e=System.GetEntityByName({}) "
			    "if e then pcall(function() System.RemoveEntity(e.id) end) end end)",
			    lua_string(name)));
		}

		std::optional<int> start_avatar_animation(
		    std::uint32_t entity_id,
		    std::string_view clip,
		    bool loop) noexcept
		{
			constexpr auto result_name =
			    "KCD2Online_RemoteAvatarAnimationResult";
			const auto script = std::format(
			    "{}=0 local ok,code=pcall(function() "
			    "local e=System.GetEntity({}) "
			    "if not e or type(e.StartAnimation)~='function' then return -1 end "
			    "local length=0 "
			    "if e.GetAnimationLength then pcall(function() "
			    "length=e:GetAnimationLength(0,{}) or 0 end) end "
			    "if length<=0 then return -4 end "
			    "local started=e:StartAnimation(0,{},0,0.15,1.0,{}) "
			    "if e.ForceCharacterUpdate then e:ForceCharacterUpdate(0,true) end "
			    "return started and 1 or -2 end) "
			    "{}=ok and code or -3",
			    result_name,
			    entity_id,
			    lua_string(clip),
			    lua_string(clip),
			    loop ? "true" : "false",
			    result_name);
			if (!execute_remote_script(script))
				return std::nullopt;
			return take_remote_script_result(result_name);
		}
	}

	native_remote_avatar_backend::native_remote_avatar_backend(
	    native_entity_backend &entities) :
	    m_entities(entities)
	{
	}

	void native_remote_avatar_backend::advance_frame() noexcept
	{
		++m_frame_sequence;
	}

	void native_remote_avatar_backend::set_epoch(std::uint64_t epoch)
	{
		if (m_epoch == epoch)
			return;
		clear();
		m_epoch = epoch;
	}

	void native_remote_avatar_backend::clear()
	{
		std::vector<remote_avatar_handle> handles;
		handles.reserve(m_avatars.size());
		for (const auto &[handle, value] : m_avatars)
		{
			(void)value;
			handles.push_back(handle);
		}
		for (const auto handle : handles)
			remove(handle);
		m_avatars.clear();
		m_probe_avatar.reset();
		m_probe_polls = 0;
	}

	void native_remote_avatar_backend::abandon_world() noexcept
	{
		// CryEngine's game-context teardown destroys these actors and their
		// inventories. Do not queue Lua removals or unequip native items here:
		// those operations may outlive this PostUpdate and race the world unload.
		m_avatars.clear();
		m_probe_avatar.reset();
		m_probe_snapshot = {};
		m_probe_polls = 0;
	}

	void native_remote_avatar_backend::reset_active_probe()
	{
		if (m_probe_avatar)
			remove(*m_probe_avatar);
		m_probe_avatar.reset();
		m_probe_snapshot = {};
		m_probe_polls = 0;
	}

	std::uint32_t native_remote_avatar_backend::entity_id_for(
	    player_id player) const noexcept
	{
		for (const auto &[handle, avatar] : m_avatars)
		{
			(void)handle;
			if (avatar.player == player && !avatar.failed)
				return avatar.entity_id;
		}
		return 0;
	}

	native_remote_avatar_backend::active_probe_result
	native_remote_avatar_backend::poll_active_probe(
	    const protocol::TransformState &origin,
	    std::string &error)
	{
		(void)origin;
		// The Lua scheduler creates a live, asynchronously initialized NPC through
		// XGen when its tool-only binding exists, or System.SpawnEntity otherwise.
		// The old CreateActor probe immediately deactivated, equipped and removed
		// that actor during world start. Reusing that lifecycle for XGen can race
		// its Human/Soul registration and tear down an entity still referenced by
		// the engine. Validate the non-mutating prerequisites here; the first real
		// remote Avatar performs the complete spawn/equipment runtime validation.
		if (!available())
		{
			error = diagnostic();
			return active_probe_result::failed;
		}
		auto *database = wh::entitymodule::C_ItemDatabase::GetInstance();
		if (!database)
		{
			error = "Avatar preflight has no native item database";
			return active_probe_result::failed;
		}
		auto &catalog = npc::runtime_equipment_catalog();
		const npc::equipment_definition *probe_equipment =
		    catalog.find(probe_equipment_definition_id);
		auto is_native_item = [database](
		                          const npc::equipment_definition *candidate)
		{
			if (!candidate)
				return false;
			CryGUID guid{};
			return wh::ParseGuid(
			           candidate->definition_id.c_str(), guid)
			    && database->FindClassByGuid(guid);
		};
		if (!is_native_item(probe_equipment))
			probe_equipment = nullptr;
		if (!probe_equipment)
		{
			for (const auto &candidate : catalog.entries())
			{
				if (candidate.equipped_slot == "PrimaryMainHand"
				    && candidate.weapon == npc::weapon_class::one_handed
				    && is_native_item(&candidate))
				{
					probe_equipment = &candidate;
					break;
				}
			}
		}
		if (!probe_equipment)
		{
			error = "Avatar preflight found no native equipment definition";
			return active_probe_result::failed;
		}
		KCD2Online_JOIN_TRACE(
		    "join.native-probe.prerequisites-ok",
		    std::format(
		        "definition_id={} xgen_spawn_deferred=true",
		        probe_equipment->definition_id));
		m_probe_polls = 0;
		error.clear();
		return active_probe_result::succeeded;
	}

	bool native_remote_avatar_backend::available() const
	{
		auto *context = wh::game::S_GameContext::GetInstance();
		auto *environment = SSystemGlobalEnvironment::GetInstance();
		KCD2Online_JOIN_TRACE(
		    "join.remote-backend.precheck",
		    std::format(
		        "game_context={} actor_system={} environment={} "
		        "entity_system={} script_system={}",
		        static_cast<void *>(context),
		        context
		            ? static_cast<void *>(context->m_pActorSystem)
		            : nullptr,
		        static_cast<void *>(environment),
		        environment
		            ? static_cast<void *>(environment->pEntitySystem)
		            : nullptr,
		        environment
		            ? static_cast<void *>(environment->pScriptSystem)
		            : nullptr));
		if (!context || !context->m_pActorSystem || !environment
		    || !environment->pEntitySystem || !environment->pScriptSystem)
		{
			m_diagnostic =
			    "native ActorSystem, EntitySystem, or ScriptSystem is unavailable";
			KCD2Online_JOIN_TRACE(
			    "join.remote-backend.unavailable",
			    m_diagnostic);
			return false;
		}
		if (!m_catalogs_ready)
		{
			std::string error;
			if (!npc::initialize_runtime_catalog(error)
			    || !npc::initialize_runtime_equipment_catalog(error))
			{
				m_diagnostic = std::move(error);
				KCD2Online_JOIN_TRACE(
				    "join.remote-backend.catalog-failed",
				    m_diagnostic);
				return false;
			}
			m_catalogs_ready = true;
			KCD2Online_JOIN_TRACE(
			    "join.remote-backend.catalog-ready",
			    "native NPC and equipment catalogs initialized and cached");
		}
		m_diagnostic.clear();
		return true;
	}

	std::string native_remote_avatar_backend::diagnostic() const
	{
		return m_diagnostic.empty()
		    ? "native remote-avatar backend is unavailable"
		    : m_diagnostic;
	}

	std::optional<remote_avatar_handle>
	native_remote_avatar_backend::spawn(
	    const remote_avatar_snapshot &player)
	{
		const auto existing = std::ranges::count_if(
		    m_avatars,
		    [&](const auto &pair)
		    {
			    return pair.second.player == player.id;
		    });
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.request",
		    std::format(
		        "player_id={} display_name=\"{}\" existing_puppets={} "
		        "has_transform={} has_avatar={} soul=\"{}\"",
		        player.id,
		        player.display_name,
		        existing,
		        player.has_transform,
		        player.has_avatar,
		        player.avatar.archetype_id()));
		if (existing != 0)
		{
			KCD2Online_JOIN_TRACE(
			    "join.remote-spawn.duplicate-detected",
			    std::format(
			        "player_id={} existing_puppets={} "
			        "spawn_is_not_a_silent_overwrite",
			        player.id,
			        existing));
		}
		if (!available())
		{
			KCD2Online_JOIN_TRACE(
			    "join.remote-spawn.rejected",
			    m_diagnostic);
			return std::nullopt;
		}
		if (!player.has_transform)
		{
			m_diagnostic = "remote avatar has no transform";
			KCD2Online_JOIN_TRACE(
			    "join.remote-spawn.rejected",
			    m_diagnostic);
			return std::nullopt;
		}
		if (!player.has_avatar)
		{
			m_diagnostic = "remote avatar has no avatar descriptor";
			KCD2Online_JOIN_TRACE(
			    "join.remote-spawn.rejected",
			    m_diagnostic);
			return std::nullopt;
		}
		if (!npc::runtime_catalog().contains(
		        player.avatar.archetype_id()))
		{
			m_diagnostic =
			    "remote avatar references an unknown native Soul";
			KCD2Online_JOIN_TRACE(
			    "join.remote-spawn.rejected",
			    std::format(
			        "soul=\"{}\" error=\"{}\"",
			        player.avatar.archetype_id(),
			        m_diagnostic));
			return std::nullopt;
		}
		const auto position = native_position(player.transform.position());
		const auto handle = m_next_handle++;
		const auto name = std::format(
		    "KCD2Online_Avatar_{}_{}_{}",
		    m_epoch,
		    player.id,
		    handle);
		const auto *archetype = npc::runtime_catalog().find(
		    player.avatar.archetype_id());
		const std::string_view class_name = archetype
		    && !archetype->archetype_name.empty()
		    ? std::string_view{archetype->archetype_name}
		    : std::string_view{"NPC"};
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.engine-call.begin",
		    std::format(
		        "api=lua-avatar-spawn name=\"{}\" "
		        "class=\"{}\" "
		        "soul=\"{}\" position=({:.6f},{:.6f},{:.6f}) "
		        "scheduler_proxy=<omitted> behavior_tree=<omitted> "
		        "entity_system={}",
		        name,
		        class_name,
		        player.avatar.archetype_id(),
		        position.x,
		        position.y,
		        position.z,
		        SSystemGlobalEnvironment::GetInstance()
		                ? static_cast<void *>(
		                      SSystemGlobalEnvironment::GetInstance()
		                          ->pEntitySystem)
		                : nullptr));
		auto spawn_authorization =
		    m_entities.authorize_human_npc_spawn(name);
		const bool queued = queue_avatar_spawn(
		    name,
		    class_name,
		    player.avatar.archetype_id(),
		    position);
		KCD2Online_JOIN_TRACE(
		    queued
		        ? "join.remote-spawn.engine-call.returned"
		        : "join.remote-spawn.engine-call.nil",
		    std::format(
		        "api=Script.SetTimer->AvatarSpawn queued={}",
		        queued));
		if (!queued)
		{
			m_diagnostic =
			    "could not queue Avatar spawn on the Lua scheduler";
			KCD2Online_JOIN_TRACE(
			    "join.remote-spawn.failed",
			    m_diagnostic);
			return std::nullopt;
		}

		const auto requested_at = std::chrono::steady_clock::now();
		const auto [iterator, inserted] = m_avatars.emplace(
		    handle,
		    entry{
		        .player = player.id,
		        .entity_name = name,
		        .spawn_requested_at = requested_at,
		        .spawn_authorization = std::move(spawn_authorization),
		        .epoch = m_epoch});
		(void)iterator;
		KCD2Online_JOIN_TRACE(
		    inserted ? "join.remote-spawn.success"
		             : "join.remote-spawn.handle-collision",
		    std::format(
		        "player_id={} handle={} entity_name=\"{}\" state=queued",
		        player.id,
		        handle,
		        name));
		if (!inserted)
		{
			m_diagnostic = "remote avatar handle collision";
			cancel_avatar_spawn(name);
			return std::nullopt;
		}
		return handle;
	}

	remote_avatar_backend_status native_remote_avatar_backend::status(
	    remote_avatar_handle avatar) const
	{
		const auto started = std::chrono::steady_clock::now();
		auto result = status_impl(avatar);
		const auto finished = std::chrono::steady_clock::now();
		const auto elapsed = finished - started;
		if (join_trace::diagnostics_enabled()
		    && elapsed >= std::chrono::milliseconds(1)
		    && (last_slow_status_diagnostic_at
		            == std::chrono::steady_clock::time_point{}
		        || finished - last_slow_status_diagnostic_at
		            >= std::chrono::seconds(1)))
		{
			last_slow_status_diagnostic_at = finished;
			join_trace::write_diagnostic(
			    "performance.remote-avatar-status",
			    std::format(
			        "handle={} state={} elapsed_ms={:.3f} diagnostic=\"{}\"",
			        avatar,
			        static_cast<int>(result.state),
			        std::chrono::duration<double, std::milli>(elapsed).count(),
			        result.diagnostic));
		}
		return result;
	}

	remote_avatar_backend_status native_remote_avatar_backend::status_impl(
	    remote_avatar_handle avatar) const
	{
		auto *value = const_cast<entry *>(find(avatar));
		KCD2Online_JOIN_TRACE(
		    "join.remote-status.begin",
		    std::format(
		        "handle={} entry={} epoch={} current_epoch={}",
		        avatar,
		        static_cast<void *>(value),
		        value ? value->epoch : 0,
		        m_epoch));
		if (!value)
			return {
			    remote_avatar_state::failed,
			    "remote avatar handle is stale"};
		if (value->failed)
			return {remote_avatar_state::failed, value->failure};
		if (value->epoch != m_epoch)
			return {
			    remote_avatar_state::failed,
			    "remote avatar belongs to a stale runtime epoch"};
		if (value->lifecycle_ready)
			return {remote_avatar_state::ready, {}};
		if (value->entity_id == 0)
		{
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			auto *system = environment ? environment->pEntitySystem : nullptr;
			auto *spawned = system && !value->entity_name.empty()
			    ? system->FindEntityByName(value->entity_name.c_str())
			    : nullptr;
			if (!spawned)
			{
				if (std::chrono::steady_clock::now()
				        - value->spawn_requested_at < remote_avatar_spawn_timeout)
				{
					return {
					    remote_avatar_state::waiting_for_human,
					    "waiting for Lua-scheduled Avatar spawn"};
				}
				value->failed = true;
				value->failure =
				    "Lua-scheduled Avatar spawn timed out";
				value->spawn_authorization.reset();
				return {remote_avatar_state::failed, value->failure};
			}

			value->entity_id = spawned->GetId();
			value->shared_soul_applied_frame = m_frame_sequence;
			value->shared_soul_applied_at =
			    std::chrono::steady_clock::now();
			m_entities.register_player_entity(
			    value->entity_id, value->player);
			value->spawn_authorization.reset();
			KCD2Online_JOIN_TRACE(
			    "join.remote-spawn.entity.resolved",
			    std::format(
			        "player_id={} entity_id={} entity_name=\"{}\" "
			        "source=lua-scheduler",
			        value->player,
			        value->entity_id,
			        value->entity_name));
		}

		auto *entity = resolve_entity(value->entity_id);
		auto *actor = resolve_actor(value->entity_id);
		KCD2Online_JOIN_TRACE(
		    "join.remote-status.pointer-state",
		    std::format(
		        "player_id={} entity_id={} entity={} actor={}",
		        value->player,
		        value->entity_id,
		        static_cast<void *>(entity),
		        static_cast<void *>(actor)));
		if (!entity)
			return {
			    remote_avatar_state::failed,
			    "remote avatar entity was destroyed externally"};
		if (!actor)
			return {
			    remote_avatar_state::waiting_for_human,
			    "waiting for XGen Human actor registration"};
		if (!actor->IsHumanActor())
			return {
			    remote_avatar_state::failed,
			    "remote avatar is not backed by a native Human"};
		// Retain only what replicated players need: mannequin/locomotion,
		// hit/condition handling and (below) Soul/Inventory.
		if (!actor->m_pMannequinStateParams
		    || !actor->m_pHitDeathReactions || !actor->m_pConditionController)
			return {
			    remote_avatar_state::waiting_for_human,
			    "waiting for native Human animation/damage controllers"};
		auto *soul = actor->m_pSoul;
		KCD2Online_JOIN_TRACE(
		    soul ? "join.remote-status.soul.ready"
		         : "join.remote-status.soul.pending",
		    std::format(
		        "player_id={} entity_id={} soul={}",
		        value->player,
		        value->entity_id,
		        static_cast<void *>(soul)));
		if (!soul)
			return {
			    remote_avatar_state::waiting_for_soul,
			    "waiting for native Soul"};
		const auto settled = evaluate_remote_soul_settle(
		    m_frame_sequence,
		    value->shared_soul_applied_frame,
		    std::chrono::steady_clock::now(),
		    value->shared_soul_applied_at);
		if (!settled.ready)
		{
			KCD2Online_JOIN_TRACE(
			    "join.remote-status.soul-settling",
			    std::format(
			        "player_id={} entity_id={} elapsed_frames={} "
			        "elapsed_ms={} required_frames={} required_ms={}",
			        value->player,
			        value->entity_id,
			        settled.elapsed_frames,
			        settled.elapsed_time.count(),
			        remote_soul_settle_frames,
			        remote_soul_settle_time.count()));
			return {
			    remote_avatar_state::stabilizing_soul,
			    "waiting for native shared-Soul stabilization"};
		}
		auto *inventory = soul->m_inventorySoul.GetInventory();
		auto *equipment =
		    soul->m_inventorySoul.GetEquipmentManager();
		KCD2Online_JOIN_TRACE(
		    "join.remote-status.inventory-state",
		    std::format(
		        "player_id={} entity_id={} inventory={} equipment_manager={}",
		        value->player,
		        value->entity_id,
		        static_cast<void *>(inventory),
		        static_cast<void *>(equipment)));
		if (!inventory || !equipment)
		{
			return {
			    remote_avatar_state::waiting_for_inventory,
			    "waiting for native Human inventory/equipment"};
		}
		value->lifecycle_ready = true;
		return {remote_avatar_state::ready, {}};
	}

	bool native_remote_avatar_backend::update(
	    remote_avatar_handle avatar,
	    const remote_avatar_snapshot &player,
	    bool appearance_changed)
	{
		const auto update_started = std::chrono::steady_clock::now();
		std::chrono::steady_clock::duration lifecycle_time{};
		std::chrono::steady_clock::duration validation_time{};
		std::chrono::steady_clock::duration transform_time{};
		std::chrono::steady_clock::duration motion_time{};
		std::chrono::steady_clock::duration appearance_time{};
		bool appearance_attempted = false;
		auto *value = find(avatar);
		if (!value || value->epoch != m_epoch || value->failed)
		{
			KCD2Online_JOIN_TRACE(
			    "join.remote-update.rejected",
			    std::format(
			        "handle={} entry={} epoch={} current_epoch={} failed={}",
			        avatar,
			        static_cast<void *>(value),
			        value ? value->epoch : 0,
			        m_epoch,
			        value ? value->failed : false));
			return false;
		}
		std::string error;
		const auto lifecycle_started = std::chrono::steady_clock::now();
		const auto lifecycle = status(avatar);
		lifecycle_time = std::chrono::steady_clock::now() - lifecycle_started;
		if (lifecycle.state == remote_avatar_state::failed)
			return false;
		// Keep this defensive even though remote_avatar_manager suppresses pending
		// updates. With the Lua-scheduled XGen path there is no entity id yet;
		// treating that as a destroyed entity would cancel/requeue the request and
		// flood the Lua timer queue. Until XGen, Human, Soul and Inventory are
		// ready, status() is the sole lifecycle owner.
		if (is_pending_remote_avatar_state(lifecycle.state))
			return true;
		const auto now = std::chrono::steady_clock::now();
		if (lifecycle.state == remote_avatar_state::ready
		    && (value->last_native_validation_at
		            == std::chrono::steady_clock::time_point{}
		        || now - value->last_native_validation_at
		            >= remote_native_validation_interval))
		{
			const auto validation_started = std::chrono::steady_clock::now();
			if (!resolve_entity(value->entity_id)
			    || !resolve_actor(value->entity_id))
			{
				value->failed = true;
				value->failure =
				    "remote avatar entity or actor was destroyed externally";
				return false;
			}
			value->last_native_validation_at = now;
			validation_time =
			    std::chrono::steady_clock::now() - validation_started;
		}
		if (lifecycle.state == remote_avatar_state::ready
		    && !apply_display_name(*value, player, error))
		{
			value->failed = true;
			value->failure = std::move(error);
			return false;
		}
		// The active ABI probe deliberately does not depend on gameplay Lua
		// contexts. Those globals are populated after Actor/Soul/Inventory are
		// already usable and were previously turning a healthy world bootstrap
		// into a fatal probe failure.
		if (lifecycle.state == remote_avatar_state::ready
		    && value->player != std::numeric_limits<player_id>::max()
		    && !value->multiplayer_semantics_applied
		    && now >= value->next_multiplayer_semantics_attempt)
		{
			const auto result = apply_multiplayer_semantics(*value, error);
			if (result == multiplayer_semantics_result::failed)
			{
				KCD2Online_JOIN_TRACE(
				    "join.remote-multiplayer-semantics.retry",
				    std::format(
				        "player_id={} entity_id={} error=\"{}\"",
				        value->player,
				        value->entity_id,
				        error));
			}
			error.clear();
		}
		if (lifecycle.state == remote_avatar_state::ready
		    && !apply_activity(*value, player, error))
		{
			value->failed = true;
			value->failure = std::move(error);
			return false;
		}
		if (!value->first_transform_logged)
		{
			value->first_transform_logged = true;
			KCD2Online_JOIN_TRACE(
			    "join.remote-update.first-transform",
			    std::format(
			        "player_id={} handle={} entity_id={} "
			        "api=IEntity::SetWorldTM position=({:.6f},{:.6f},{:.6f})",
			        value->player,
			        avatar,
			        value->entity_id,
			        player.transform.position().x(),
			        player.transform.position().y(),
			        player.transform.position().z()));
		}
		const bool activity_locked = value->activity_active;
		const bool movement_stopped = value->motion_applied
		    && value->last_movement_mode != protocol::MOVEMENT_MODE_IDLE
		    && player.movement_mode == protocol::MOVEMENT_MODE_IDLE;
		const bool transform_changed = !activity_locked
		    && (!value->transform_applied
		    || movement_stopped
		    || position_or_rotation_changed(
		        value->last_transform,
		        player.transform)
		    || now - value->last_native_transform_at
		        >= std::chrono::milliseconds(16));
		bool transform_succeeded = true;
		if (transform_changed)
		{
			const auto transform_started = std::chrono::steady_clock::now();
			auto *entity = resolve_entity(value->entity_id);
			if (!entity)
				error = "native remote entity disappeared";
			transform_succeeded = entity
			    && m_entities.write_transform(
			        entity,
			        player.transform,
			        error);
			if (transform_succeeded)
				value->last_native_transform_at = transform_started;
			transform_time =
			    std::chrono::steady_clock::now() - transform_started;
		}
		bool motion_succeeded = true;
		if (transform_succeeded && !activity_locked)
		{
			const auto motion_started = std::chrono::steady_clock::now();
			motion_succeeded = update_motion_state(*value, player, error);
			motion_time = std::chrono::steady_clock::now() - motion_started;
		}
		update_animation_state(*value, player);
		if (!transform_succeeded || !motion_succeeded)
		{
			value->failed = true;
			value->failure = std::move(error);
			KCD2Online_JOIN_TRACE(
			    "join.remote-update.failed",
			    std::format(
			        "player_id={} handle={} entity_id={} error=\"{}\"",
			        value->player,
			        avatar,
			        value->entity_id,
			        value->failure));
			return false;
		}
		if (transform_changed)
		{
			value->last_transform = player.transform;
			value->transform_applied = true;
		}

		if (lifecycle.state == remote_avatar_state::ready
		    && (appearance_changed || !value->appearance_applied))
		{
			appearance_attempted = true;
			const auto appearance_started = std::chrono::steady_clock::now();
			const auto old = value->appearance;
			const bool had_old = value->appearance_applied;
			if (!apply_appearance(*value, player.avatar, error))
			{
				std::string rollback_error;
				const bool restored = !had_old
				    ? remove_created_items(*value, rollback_error)
				    : apply_appearance(
				        *value,
				        old,
				        rollback_error);
				value->failed = true;
				value->failure =
				    "remote equipment transaction failed: " + error;
				if (!restored)
					value->failure +=
					    "; rollback failed: " + rollback_error;
				return false;
			}
			value->appearance = player.avatar;
			value->appearance_applied = true;
			appearance_time =
			    std::chrono::steady_clock::now() - appearance_started;
		}
		// XGen owns Human/Soul/physics initialization. Presentation is a logical
		// readiness boundary only; it does not mutate the XGen lifecycle.
		if (lifecycle.state == remote_avatar_state::ready
		    && value->player != std::numeric_limits<player_id>::max()
		    && !value->presented
		    && !present(*value, error))
		{
			value->failed = true;
			value->failure = std::move(error);
			KCD2Online_JOIN_TRACE(
			    "join.remote-presentation.failed",
			    std::format(
			        "player_id={} handle={} entity_id={} error=\"{}\"",
			        value->player,
			        avatar,
			        value->entity_id,
			        value->failure));
			return false;
		}
		// This is the only FullBody-layer write in an Avatar update. Equipment
		// and weapon tags have settled already; activity owns the body while it is
		// active, otherwise an emote wins over locomotion.
		if (transform_succeeded && value->presented
		    && !present_animation(*value, player, error))
		{
			value->failed = true;
			value->failure = std::move(error);
			return false;
		}
		const auto update_finished = std::chrono::steady_clock::now();
		if (join_trace::diagnostics_enabled()
		    && (last_update_diagnostic_at
		        == std::chrono::steady_clock::time_point{}
		    || update_finished - last_update_diagnostic_at
		        >= std::chrono::seconds(1)))
		{
			last_update_diagnostic_at = update_finished;
			const auto milliseconds = [](auto duration)
			{
				return std::chrono::duration<double, std::milli>(duration).count();
			};
			Vec3 actual_position{};
			bool actual_position_available{};
			if (auto *entity = resolve_entity(value->entity_id))
			{
				if (const auto *matrix = entity->GetWorldTMPtr())
				{
					actual_position = matrix->GetTranslation();
					actual_position_available = true;
				}
			}
			join_trace::write_diagnostic(
			    "performance.remote-avatar-update",
			    std::format(
			        "player_id={} handle={} sequence={} total_ms={:.3f} "
			        "lifecycle_ms={:.3f} validation_ms={:.3f} "
			        "transform_changed={} transform_ms={:.3f} "
			        "motion_ms={:.3f} appearance_attempted={} "
			        "appearance_ms={:.3f} target=({:.3f},{:.3f},{:.3f}) "
			        "actual_available={} actual=({:.3f},{:.3f},{:.3f}) "
			        "locomotion_clip=\"{}\" visual_speed={:.3f} one_shot={}",
			        value->player,
			        avatar,
			        player.transform.sequence(),
			        milliseconds(update_finished - update_started),
			        milliseconds(lifecycle_time),
			        milliseconds(validation_time),
			        transform_changed,
			        milliseconds(transform_time),
			        milliseconds(motion_time),
			        appearance_attempted,
			        milliseconds(appearance_time),
			        player.transform.position().x(),
			        player.transform.position().y(),
			        player.transform.position().z(),
			        actual_position_available,
			        actual_position.x,
			        actual_position.y,
			        actual_position.z,
			        remote_locomotion_animation_name(
			            value->locomotion_animation),
			        value->smoothed_visual_speed,
			        value->one_shot_animation_active));
		}
		return true;
	}

	bool native_remote_avatar_backend::present(
	    entry &avatar,
	    std::string &error)
	{
		if (avatar.presented)
			return true;
		auto *entity = resolve_entity(avatar.entity_id);
		if (!entity)
		{
			error = "native remote entity disappeared before presentation";
			return false;
		}

		KCD2Online_JOIN_TRACE(
		    "join.remote-presentation.begin",
		    std::format(
		        "player_id={} entity_id={} xgen_state=preserved",
		        avatar.player,
		        avatar.entity_id));
		avatar.presented = true;
		avatar.motion_applied = false;
		KCD2Online_JOIN_TRACE(
		    "join.remote-presentation.complete",
		    std::format(
		        "player_id={} entity_id={}",
		        avatar.player,
		        avatar.entity_id));
		return true;
	}

	bool native_remote_avatar_backend::apply_display_name(
	    entry &avatar,
	    const remote_avatar_snapshot &player,
	    std::string &error)
	{
		if (avatar.display_name_applied
		    && avatar.display_name == player.display_name)
		{
			return true;
		}
		if (player.display_name.empty())
		{
			error = "remote player has no display name";
			return false;
		}
		auto *actor = resolve_actor(avatar.entity_id);
		auto *soul = actor ? actor->m_pSoul : nullptr;
		if (!soul)
		{
			error = "remote Soul is unavailable for display-name assignment";
			return false;
		}

		auto *native_name = reinterpret_cast<CryStringT<char> *>(
		    reinterpret_cast<std::uintptr_t>(soul)
		    + soul_display_name_string_id_offset);
		native_name->assign(player.display_name.c_str());
		avatar.display_name = player.display_name;
		avatar.display_name_applied = true;
		KCD2Online_JOIN_TRACE(
		    "join.remote-display-name.applied",
		    std::format(
		        "player_id={} entity_id={} display_name=\"{}\"",
		        avatar.player,
		        avatar.entity_id,
		        avatar.display_name));
		return true;
	}

	native_remote_avatar_backend::multiplayer_semantics_result
	native_remote_avatar_backend::apply_multiplayer_semantics(
	    entry &avatar,
	    std::string &error)
	{
		avatar.next_multiplayer_semantics_attempt =
		    std::chrono::steady_clock::now() + std::chrono::seconds(1);
		if (!remote_semantics_api_ready())
		{
			error.clear();
			return multiplayer_semantics_result::deferred;
		}
		const auto script = std::format(
		    "KCD2Online_RemoteAvatarPolicyResult=0 "
		    "local ok,code=pcall(function() "
		    "local e=System.GetEntity({}) "
		    "if not e then return 2 end "
		    "if not e.actor or not e.human or not e.soul then return 3 end "
		    "if type(e.actor.IsUnconscious)~='function' "
		    "or type(e.human.CanBeRobbed)~='function' "
		    "or type(e.soul.RestrictDialog)~='function' "
		    "or type(e.soul.HasScriptContext)~='function' then return 4 end "
		    "local disabled={{'switch_disabledInformationReaction',"
		    "'switch_disabledHearingReaction',"
		    "'switch_disabledPerceptionReaction',"
		    "'switch_disabledPickpocketReaction',"
		    "'switch_disabledNearMissReaction',"
		    "'switch_disabledHitBehavioralReaction',"
		    "'crime_disableReport'}} "
		    "for _,context in ipairs(disabled) do "
		    "Contexts.SetPersistentOption(e,context,'KCD2OnlineRemotePlayer') end "
		    "e.soul:RestrictDialog(true) "
		    "if e.human.InterruptDialogs then e.human:InterruptDialogs() end "
		    "for _,context in ipairs(disabled) do "
		    "if not e.soul:HasScriptContext(context) then return 5 end end "
		    "return 1 end) "
		    "KCD2Online_RemoteAvatarPolicyResult=ok and code or -1",
		    avatar.entity_id);
		if (!execute_remote_script(script))
		{
			error =
			    "could not isolate remote player from vanilla "
			    "pickpocket/dialogue reactions";
			return multiplayer_semantics_result::failed;
		}
		const auto result = take_remote_semantics_result();
		if (!result || *result != 1)
		{
			error = std::format(
			    "remote avatar policy validation is not ready (code={})",
			    result.value_or(-2));
			return !result || *result >= 2
			    ? multiplayer_semantics_result::deferred
			    : multiplayer_semantics_result::failed;
		}
		avatar.multiplayer_semantics_applied = true;
		avatar.next_multiplayer_semantics_attempt = {};
		KCD2Online_JOIN_TRACE(
		    "join.remote-multiplayer-semantics.applied",
		    std::format(
		        "player_id={} entity_id={} "
		        "autonomous_reactions=false hit_reaction=true "
		        "pickpocket=true knockout=true dialog_restricted=true",
		        avatar.player,
		        avatar.entity_id));
		return multiplayer_semantics_result::applied;
	}

	bool native_remote_avatar_backend::apply_activity(
	    entry &avatar,
	    const remote_avatar_snapshot &player,
	    std::string &error)
	{
		const bool active = player.has_activity && player.activity.active();
		if (!active && !avatar.activity_active)
			return true;
		if (!active)
		{
			// The next single-owner presentation decision transitions directly
			// back to locomotion. Do not enqueue a second, untracked exit clip.
			avatar.activity_active = false;
			avatar.activity_kind = protocol::PLAYER_ACTIVITY_KIND_NONE;
			avatar.activity_session_id = 0;
			avatar.activity_station_guid = 0;
			avatar.motion_applied = false;
			avatar.transform_applied = false;
			return true;
		}
		if (active && avatar.activity_active
		    && avatar.activity_session_id == player.activity.session_id())
		{
			return true;
		}

		auto *environment = SSystemGlobalEnvironment::GetInstance();
		if (!environment || !environment->pEntitySystem)
		{
			error = "entity system is unavailable for remote activity";
			return false;
		}
		const auto station_guid = player.activity.station_guid();
		const auto station_id = environment->pEntitySystem->FindEntityByGuid(
		    station_guid);
		if (station_id == 0)
		{
			error = "remote activity station is not loaded in this level";
			return false;
		}

		std::string_view action;
		if (active)
		{
			switch (player.activity.kind())
			{
			case protocol::PLAYER_ACTIVITY_KIND_BLACKSMITHING:
				action = "SmithHeating";
				break;
			case protocol::PLAYER_ACTIVITY_KIND_SHARPENING:
				action = "CampSharpeningSwordLoop_VAR";
				break;
			case protocol::PLAYER_ACTIVITY_KIND_ALCHEMY:
				action = "Alchemy";
				break;
			case protocol::PLAYER_ACTIVITY_KIND_NONE:
			default:
				error = "remote activity kind has no animation mapping";
				return false;
			}
		}
		if (!action.empty())
		{
			const auto script = std::format(
			    "local e=System.GetEntity({}) if e and e.actor then "
			    "e.actor:StartInteractiveActionByName('{}',{},true,1) end",
			    avatar.entity_id,
			    action,
			    station_id);
			if (!execute_remote_script(script))
			{
				error = "remote activity animation could not be started";
				return false;
			}
		}

		avatar.activity_active = true;
		avatar.activity_kind = player.activity.kind();
		avatar.activity_session_id = player.activity.session_id();
		avatar.activity_station_guid = player.activity.station_guid();
		return true;
	}

	void native_remote_avatar_backend::remove(
	    remote_avatar_handle avatar)
	{
		const auto found = m_avatars.find(avatar);
		if (found == m_avatars.end())
			return;
		const auto id = found->second.entity_id;
		cancel_avatar_spawn(found->second.entity_name);
		std::string ignored;
		(void)remove_created_items(found->second, ignored);
		if (id != 0)
			m_entities.unregister_player_entity(id);
		queue_avatar_remove(found->second.entity_name);
		m_avatars.erase(found);
	}

	native_remote_avatar_backend::entry *
	native_remote_avatar_backend::find(remote_avatar_handle avatar)
	{
		const auto found = m_avatars.find(avatar);
		return found == m_avatars.end() ? nullptr : &found->second;
	}

	const native_remote_avatar_backend::entry *
	native_remote_avatar_backend::find(remote_avatar_handle avatar) const
	{
		const auto found = m_avatars.find(avatar);
		return found == m_avatars.end() ? nullptr : &found->second;
	}

	bool native_remote_avatar_backend::remove_created_items(
	    entry &avatar,
	    std::string &error)
	{
		auto *actor = resolve_actor(avatar.entity_id);
		auto *soul = actor ? actor->m_pSoul : nullptr;
		auto *inventory =
		    soul ? soul->m_inventorySoul.GetInventory() : nullptr;
		if (!inventory)
		{
			if (avatar.item_instances.empty())
				return true;
			error = "remote inventory disappeared during cleanup";
			return false;
		}
		return clear_native_remote_equipment(
		    *soul,
		    *inventory,
		    avatar.item_instances,
		    error);
	}

	bool native_remote_avatar_backend::apply_appearance(
	    entry &avatar,
	    const protocol::AvatarDescriptor &appearance,
	    std::string &error)
	{
		auto *actor = resolve_actor(avatar.entity_id);
		auto *soul = actor ? actor->m_pSoul : nullptr;
		auto *inventory =
		    soul ? soul->m_inventorySoul.GetInventory() : nullptr;
		auto *database = wh::entitymodule::C_ItemDatabase::GetInstance();
		KCD2Online_JOIN_TRACE(
		    "join.remote-appearance.precheck",
		    std::format(
		        "player_id={} entity_id={} actor={} soul={} inventory={} "
		        "item_database={} equipment_count={}",
		        avatar.player,
		        avatar.entity_id,
		        static_cast<void *>(actor),
		        static_cast<void *>(soul),
		        static_cast<void *>(inventory),
		        static_cast<void *>(database),
		        appearance.equipment_size()));
		if (!actor || !soul || !inventory || !database)
		{
			error = "remote Human/Soul/Inventory readiness was lost";
			return false;
		}

		auto *human =
		    reinterpret_cast<wh::entitymodule::C_Human *>(actor);
		const bool rebuild_equipment = !avatar.appearance_applied
		    || !same_avatar_equipment(avatar.appearance, appearance);
		if (rebuild_equipment)
		{
			const native_remote_equipment_context native{
			    avatar.player,
			    avatar.entity_id,
			    *human,
			    *soul,
			    *inventory,
			    *database};
			if (!apply_native_remote_equipment(
			        native,
			        appearance,
			        avatar.item_instances,
			        error))
			{
				return false;
			}
		}

		const bool should_draw = avatar_weapon_should_draw(appearance);
		if (avatar.native_weapon_actions_enabled
		    && !native_avatar_weapon_state_matches(*human, appearance))
		{
			if (!avatar.first_weapon_action_logged)
			{
				avatar.first_weapon_action_logged = true;
				KCD2Online_JOIN_TRACE(
				    "join.remote-animation.first-weapon-action",
				    std::format(
				        "player_id={} entity_id={} requested_drawn={} "
				        "weapon_class={} weapon_set={} "
				        "api=native_weapon_controller",
				        avatar.player,
				        avatar.entity_id,
				        should_draw,
				        static_cast<int>(appearance.weapon_class()),
				        static_cast<int>(appearance.active_weapon_set())));
			}
			KCD2Online_JOIN_TRACE(
			    "join.remote-animation.weapon-action.begin",
			    std::format(
			        "player_id={} entity_id={} requested_drawn={}",
			        avatar.player,
			        avatar.entity_id,
			        should_draw));
			const auto result =
			    guarded_apply_weapon_state(*human, appearance);
			if (result != weapon_action_result::applied)
			{
				// Weapon presentation is optional for a replicated visual. A
				// rejected or faulting native controller must not destroy the
				// remote avatar and unload the local multiplayer world.
				avatar.native_weapon_actions_enabled = false;
				KCD2Online_JOIN_TRACE(
				    "join.remote-animation.weapon-action.disabled",
				    std::format(
				        "player_id={} entity_id={} requested_drawn={} reason={}",
				        avatar.player,
				        avatar.entity_id,
				        should_draw,
				        result == weapon_action_result::faulted
				            ? "seh"
				            : "rejected"));
				return true;
			}
			KCD2Online_JOIN_TRACE(
			    "join.remote-animation.weapon-action.returned",
			    std::format(
			        "player_id={} entity_id={} result=true",
			        avatar.player,
			        avatar.entity_id));
		}
		return true;
	}

	bool native_remote_avatar_backend::update_motion_state(
	    entry &avatar,
	    const remote_avatar_snapshot &player,
	    std::string &error)
	{
		const auto now = std::chrono::steady_clock::now();
		auto *entity = resolve_entity(avatar.entity_id);
		if (!avatar.first_motion_logged)
		{
			avatar.first_motion_logged = true;
			KCD2Online_JOIN_TRACE(
			    "join.remote-animation.first-locomotion",
			    std::format(
			        "player_id={} entity_id={} entity={} movement_mode={} "
			        "api=rendered-position-state",
			        avatar.player,
			        avatar.entity_id,
			        static_cast<void *>(entity),
			        static_cast<int>(player.movement_mode)));
		}
		if (!entity)
		{
			error = "native remote entity disappeared before locomotion animation";
			return false;
		}

		float network_speed = std::hypot(
		    player.transform.velocity().x(),
		    player.transform.velocity().y());
		if (player.transform.has_locomotion())
			network_speed = player.transform.locomotion().speed();
		if (network_speed < 0.05F)
		{
			switch (player.movement_mode)
			{
			case protocol::MOVEMENT_MODE_WALK:
				network_speed = 1.5F;
				break;
			case protocol::MOVEMENT_MODE_RUN:
				network_speed = 3.8F;
				break;
			case protocol::MOVEMENT_MODE_SPRINT:
				network_speed = 5.2F;
				break;
			case protocol::MOVEMENT_MODE_IDLE:
			default:
				break;
			}
		}

		float target_speed = network_speed;
		if (const auto *matrix = entity->GetWorldTMPtr())
		{
			const auto position = matrix->GetTranslation();
			if (avatar.visual_position_sampled)
			{
				const auto elapsed = std::chrono::duration<float>(
				    now - avatar.last_visual_sample_at).count();
				const auto dx = position.x - avatar.last_visual_x;
				const auto dy = position.y - avatar.last_visual_y;
				const auto distance = std::hypot(dx, dy);
				// Ignore teleports, world transitions and long frame stalls. In those
				// cases the replicated movement mode is a safer visual hint than the
				// apparent one-frame speed.
				if (elapsed >= 0.005F && elapsed <= 0.25F && distance <= 2.0F)
					target_speed = distance / elapsed;
			}
			else
			{
				avatar.smoothed_visual_speed = network_speed;
				avatar.locomotion_animation =
				    remote_locomotion_animation_for_mode(
				        player.movement_mode);
			}
			avatar.last_visual_x = position.x;
			avatar.last_visual_y = position.y;
			avatar.last_visual_sample_at = now;
			avatar.visual_position_sampled = true;
		}

		target_speed = std::clamp(target_speed, 0.0F, 12.0F);
		// Match the working Ghost implementation: animation state is derived
		// from what this client actually rendered, with a simple 25% low-pass.
		avatar.smoothed_visual_speed = avatar.visual_position_sampled
		    ? avatar.smoothed_visual_speed * 0.75F + target_speed * 0.25F
		    : target_speed;
		const auto desired = select_remote_locomotion_animation(
		    avatar.smoothed_visual_speed,
		    avatar.locomotion_animation);
		avatar.locomotion_animation = desired;
		avatar.last_movement_mode = player.movement_mode;
		return true;
	}

	void native_remote_avatar_backend::update_animation_state(
	    entry &avatar,
	    const remote_avatar_snapshot &player)
	{
		if (!player.transform.has_animation())
			return;
		const auto &animation = player.transform.animation();
		if (animation.sequence() <= avatar.last_animation_sequence)
			return;
		avatar.last_animation_sequence = animation.sequence();
		const auto *emote = find_emote_fragment(animation.fragment());
		if (animation.active() && !emote)
		{
			KCD2Online_JOIN_TRACE(
			    "join.remote-animation.skeleton-rejected",
			    std::format(
			        "player_id={} entity_id={} sequence={} fragment=\"{}\" reason=unknown-emote",
			        avatar.player,
			        avatar.entity_id,
			        animation.sequence(),
			        animation.fragment()));
			avatar.one_shot_animation_active = false;
			avatar.one_shot_animation_clip.clear();
			avatar.motion_applied = false;
			return;
		}
		avatar.one_shot_animation_active = animation.active();
		avatar.one_shot_animation_clip = animation.active()
		    ? std::string{emote->skeleton_clip}
		    : std::string{};
		avatar.motion_applied = false;
		avatar.next_motion_retry_at = {};
		KCD2Online_JOIN_TRACE(
		    "join.remote-animation.state-accepted",
		    std::format(
		        "player_id={} entity_id={} sequence={} active={} "
		        "fragment=\"{}\" clip=\"{}\" owner={}",
		        avatar.player,
		        avatar.entity_id,
		        animation.sequence(),
		        animation.active(),
		        animation.fragment(),
		        emote ? emote->skeleton_clip : std::string_view{},
		        animation.active() ? "emote" : "locomotion"));
	}

	bool native_remote_avatar_backend::present_animation(
	    entry &avatar,
	    const remote_avatar_snapshot &player,
	    std::string &error)
	{
		if (avatar.activity_active)
			return true;
		const auto now = std::chrono::steady_clock::now();
		if (now < avatar.next_motion_retry_at)
			return true;

		const bool loop = !avatar.one_shot_animation_active;
		const auto locomotion = avatar.locomotion_animation
		        == remote_locomotion_animation::sprint
		    && !avatar.sprint_animation_supported
		    ? remote_locomotion_animation::run
		    : avatar.locomotion_animation;
		const std::string_view clip = avatar.one_shot_animation_active
		    ? std::string_view{avatar.one_shot_animation_clip}
		    : remote_locomotion_animation_name(locomotion);
		if (clip.empty())
		{
			error = "Avatar presentation selected an empty animation clip";
			return false;
		}

		const bool changed = avatar.presented_animation_clip != clip
		    || avatar.presented_animation_loop != loop;
		const auto result = start_avatar_animation(
		    avatar.entity_id, clip, loop);
		// CryAnimation normally rejects re-adding the same FIFO entry unless the
		// restart flag is set. That is the behavior we want: the call reasserts
		// ownership without resetting foot phase. An already validated, unchanged
		// clip is therefore still healthy when the binding reports "not started".
		const bool accepted = result && (*result == 1
		    || (*result == -2 && !changed));
		if (!accepted)
		{
			avatar.motion_applied = false;
			if (result && *result == -4 && loop
			    && avatar.locomotion_animation
			        == remote_locomotion_animation::sprint)
			{
				avatar.sprint_animation_supported = false;
				avatar.next_motion_retry_at = {};
			}
			else if (result && *result == -4 && !loop)
			{
				// A catalog emote absent from this concrete skeleton cannot own
				// FullBody. Resume locomotion on the next Avatar update.
				avatar.one_shot_animation_active = false;
				avatar.one_shot_animation_clip.clear();
				avatar.next_motion_retry_at = {};
			}
			else
			{
				avatar.next_motion_retry_at = now + remote_motion_retry;
			}
			KCD2Online_JOIN_TRACE(
			    "join.remote-animation.skeleton-retry",
			    std::format(
			        "player_id={} entity_id={} owner={} clip=\"{}\" "
			        "validated_result={} retry_ms={}",
			        avatar.player,
			        avatar.entity_id,
			        loop ? "locomotion" : "emote",
			        clip,
			        result.value_or(-4),
			        remote_motion_retry.count()));
			return true;
		}

		avatar.next_motion_retry_at = {};
		avatar.presented_animation_clip = clip;
		avatar.presented_animation_loop = loop;
		avatar.last_motion_request_at = now;
		avatar.motion_applied = loop;
		if (changed)
		{
			KCD2Online_JOIN_TRACE(
			    "join.remote-animation.skeleton-applied",
			    std::format(
			        "player_id={} entity_id={} movement_mode={} speed={:.3f} "
			        "owner={} clip=\"{}\" loop={} validated=true",
			        avatar.player,
			        avatar.entity_id,
			        static_cast<int>(player.movement_mode),
			        avatar.smoothed_visual_speed,
			        loop ? "locomotion" : "emote",
			        clip,
			        loop));
		}
		return true;
	}
}
