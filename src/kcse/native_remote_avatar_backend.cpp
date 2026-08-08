#include "kcse/native_remote_avatar_backend.hpp"
#include "kcse/native_avatar_combat.hpp"
#include "kcse/native_inventory.hpp"
#include "kcse/native_remote_avatar_equipment.hpp"
#include "kcse/join_trace.hpp"
#include "multiplayer/avatar_visual.hpp"

#include "npc/equipment_catalog.hpp"

#include <entitymodule/C_Actor.h>
#include <entitymodule/C_Human.h>
#include <entitymodule/C_Inventory.h>
#include <entitymodule/C_Item.h>
#include <entitymodule/C_ItemDatabase.h>
#include <framework/GuidUtils.h>
#include <game/S_GameContext.h>
#include <rpgmodule/C_Soul.h>
#include <rpgmodule/C_SoulList.h>
#include <crysystem/CEntity.h>
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
		constexpr std::uint32_t entity_flag_calc_physics = 1U << 7;
		constexpr std::uint32_t entity_flag_client_only = 1U << 8;
		constexpr std::uint32_t entity_flag_has_ai = 1U << 13;
		constexpr std::uint32_t entity_flag_trigger_areas = 1U << 14;
		constexpr std::uint32_t entity_flag_no_save = 1U << 15;
		constexpr std::uint32_t entity_flag_clientside_state = 1U << 17;
		constexpr std::uint32_t entity_flag_no_proximity = 1U << 19;
		constexpr std::uint32_t entity_flag_never_network_static = 1U << 22;
		constexpr std::uint32_t remote_actor_creation_flags =
		    entity_flag_client_only | entity_flag_no_save
		    | entity_flag_clientside_state
		    | entity_flag_never_network_static;
		// Soul.GetNameStringId() reads the CryString stored at C_Soul+0x3E8.
		// This is the name consumed by the target/interaction HUD; IEntity::GetName()
		// is only the engine's technical entity identifier.
		constexpr std::uintptr_t soul_display_name_string_id_offset = 0x3E8;
		constexpr auto remote_motion_keepalive =
		    std::chrono::milliseconds(250);
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

		bool velocity_changed(
		    const protocol::Vec3 &left,
		    const protocol::Vec3 &right)
		{
			constexpr float velocity_epsilon_squared = 0.0025F;
			const auto dx = left.x() - right.x();
			const auto dy = left.y() - right.y();
			const auto dz = left.z() - right.z();
			return dx * dx + dy * dy + dz * dz
			    > velocity_epsilon_squared;
		}

		bool locomotion_changed(
		    const protocol::LocomotionState &left,
		    const protocol::LocomotionState &right)
		{
			return std::abs(left.speed() - right.speed()) > 0.05F
			    || std::abs(left.yaw_rate() - right.yaw_rate()) > 0.05F
			    || left.strafing() != right.strafing()
			    || velocity_changed(
			        left.local_velocity(), right.local_velocity())
			    || velocity_changed(
			        left.facing_direction(), right.facing_direction());
		}

		Quat native_rotation(const protocol::Quaternion &rotation)
		{
			return Quat(
			    rotation.w(),
			    rotation.x(),
			    rotation.y(),
			    rotation.z());
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

		enum class locomotion_request_result
		{
			applied,
			rejected,
			faulted
		};

		locomotion_request_result guarded_request_locomotion(
		    wh::entitymodule::C_Actor &actor,
		    const SMultiplayerLocomotionRequest &request) noexcept
		{
#ifdef _WIN32
			__try
			{
				return actor.RequestLocomotion(request)
				    ? locomotion_request_result::applied
				    : locomotion_request_result::rejected;
			}
			__except(KCD2Online_JOIN_SEH_FILTER(
			    "join.remote-animation.locomotion.seh"))
			{
				return locomotion_request_result::faulted;
			}
#else
			return actor.RequestLocomotion(request)
			    ? locomotion_request_result::applied
			    : locomotion_request_result::rejected;
#endif
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

		protocol::TransformState smooth_transform_target(
		    Offsets::IEntity &entity,
		    const protocol::TransformState &target,
		    float seconds,
		    bool &needs_write)
		{
			auto result = target;
			const auto *matrix = entity.GetWorldTMPtr();
			if (!matrix)
			{
				needs_write = true;
				return result;
			}
			const protocol::Vec3 current_position = [&]
			{
				protocol::Vec3 value;
				value.set_x(matrix->m03);
				value.set_y(matrix->m13);
				value.set_z(matrix->m23);
				return value;
			}();
			const auto dx = target.position().x() - current_position.x();
			const auto dy = target.position().y() - current_position.y();
			const auto dz = target.position().z() - current_position.z();
			const auto error = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (error > 5.0F)
			{
				needs_write = true;
				return result;
			}

			needs_write = true;
			const auto factor = std::clamp(
			    1.0F - std::exp(-14.0F * std::clamp(seconds, 0.0F, 0.1F)),
			    0.08F,
			    0.7F);
			auto *position = result.mutable_position();
			if (error < 0.025F)
				*position = current_position;
			else
			{
				position->set_x(current_position.x() + dx * factor);
				position->set_y(current_position.y() + dy * factor);
				position->set_z(current_position.z() + dz * factor);
			}

			const Quat current(*matrix);
			const auto &desired = target.rotation();
			auto dot = current.v.x * desired.x()
			    + current.v.y * desired.y()
			    + current.v.z * desired.z()
			    + current.w * desired.w();
			if (error < 0.025F && std::abs(dot) > 0.99995F)
			{
				needs_write = false;
				return result;
			}
			const auto sign = dot < 0.0F ? -1.0F : 1.0F;
			auto *rotation = result.mutable_rotation();
			rotation->set_x(current.v.x + (desired.x() * sign - current.v.x) * factor);
			rotation->set_y(current.v.y + (desired.y() * sign - current.v.y) * factor);
			rotation->set_z(current.v.z + (desired.z() * sign - current.v.z) * factor);
			rotation->set_w(current.w + (desired.w() * sign - current.w) * factor);
			(void)normalize_rotation(rotation);
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

		Offsets::IActor *guarded_create_actor(
		    Offsets::IActorSystem *actor_system,
		    const char *name,
		    const Vec3 *position,
		    const Quat *rotation,
		    const Vec3 *scale) noexcept
		{
#ifdef _WIN32
			__try
			{
				return actor_system->CreateActor(
				    0,
				    name,
				    "NPC",
				    position,
				    rotation,
				    scale,
				    remote_actor_creation_flags);
			}
			__except(KCD2Online_JOIN_SEH_FILTER(
			    "join.remote-spawn.CreateActor.seh"))
			{
				return nullptr;
			}
#else
			return actor_system->CreateActor(
			    0,
			    name,
			    "NPC",
			    position,
			    rotation,
			    scale,
			    remote_actor_creation_flags);
#endif
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

	void native_remote_avatar_backend::reset_active_probe()
	{
		if (m_probe_avatar)
			remove(*m_probe_avatar);
		m_probe_avatar.reset();
		m_probe_snapshot = {};
		m_probe_polls = 0;
	}

	native_remote_avatar_backend::active_probe_result
	native_remote_avatar_backend::poll_active_probe(
	    const protocol::TransformState &origin,
	    std::string &error)
	{
		if (!m_probe_avatar)
		{
			if (!available())
			{
				error = diagnostic();
				return active_probe_result::failed;
			}
			auto *database = wh::entitymodule::C_ItemDatabase::GetInstance();
			if (!database)
			{
				error = "active probe has no native item database";
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
				           candidate->definition_id.c_str(),
				           guid)
				    && database->FindClassByGuid(guid);
			};
			if (!is_native_item(probe_equipment))
				probe_equipment = nullptr;
			if (!probe_equipment)
			{
				for (const auto &candidate : catalog.entries())
				{
					if (candidate.equipped_slot == "PrimaryMainHand"
					    && candidate.weapon
					        == npc::weapon_class::one_handed
					    && is_native_item(&candidate))
					{
						probe_equipment = &candidate;
						break;
					}
				}
			}
			if (!probe_equipment)
			{
				error =
				    "active probe found no native equipment definition";
				return active_probe_result::failed;
			}
			KCD2Online_JOIN_TRACE(
			    "join.native-probe.equipment-selected",
			    std::format(
			        "definition_id={} equipped_slot={} weapon_class={}",
			        probe_equipment->definition_id,
			        probe_equipment->equipped_slot,
			        static_cast<int>(probe_equipment->weapon)));

			m_probe_snapshot = {};
			m_probe_snapshot.id =
			    std::numeric_limits<std::uint64_t>::max();
			m_probe_snapshot.display_name = "KCD2Online native ABI probe";
			m_probe_snapshot.connected = true;
			m_probe_snapshot.has_transform = true;
			m_probe_snapshot.transform = origin;
			m_probe_snapshot.movement_mode =
			    protocol::MOVEMENT_MODE_IDLE;
			m_probe_snapshot.has_avatar = true;
			m_probe_snapshot.avatar.set_archetype_id(
			    npc::default_soul_id);
			m_probe_snapshot.avatar.set_revision(1);
			m_probe_snapshot.avatar.set_stance(
			    protocol::AVATAR_STANCE_RELAXED);
			m_probe_snapshot.avatar.set_weapon_class(
			    protocol_weapon_class(probe_equipment->weapon));
			m_probe_snapshot.avatar.set_weapon_drawn(false);
			auto *equipment =
			    m_probe_snapshot.avatar.add_equipment();
			equipment->set_definition_id(
			    probe_equipment->definition_id);
			equipment->set_equipped_slot(
			    probe_equipment->equipped_slot);

			m_probe_avatar = spawn(m_probe_snapshot);
			if (!m_probe_avatar)
			{
				error = diagnostic();
				return active_probe_result::failed;
			}
			if (auto *value = find(*m_probe_avatar))
			{
				if (auto *entity = resolve_entity(value->entity_id))
				{
					entity->Activate(false);
					entity->Hide(true);
				}
			}
			return active_probe_result::pending;
		}

		if (++m_probe_polls > 600)
		{
			error = "active native Avatar probe timed out";
			reset_active_probe();
			return active_probe_result::failed;
		}
		const auto lifecycle = status(*m_probe_avatar);
		if (is_pending_remote_avatar_state(lifecycle.state))
			return active_probe_result::pending;
		if (lifecycle.state == remote_avatar_state::failed)
		{
			error = lifecycle.diagnostic;
			reset_active_probe();
			return active_probe_result::failed;
		}
		if (!update(*m_probe_avatar, m_probe_snapshot, true))
		{
			error = diagnostic();
			if (const auto *value = find(*m_probe_avatar);
			    value && !value->failure.empty())
				error = value->failure;
			reset_active_probe();
			return active_probe_result::failed;
		}

		auto *value = find(*m_probe_avatar);
		auto *actor = value ? resolve_actor(value->entity_id) : nullptr;
		auto *soul = actor ? actor->m_pSoul : nullptr;
		auto *inventory =
		    soul ? soul->m_inventorySoul.GetInventory() : nullptr;
		if (!value || !inventory || value->item_instances.size() != 1)
		{
			error =
			    "active native probe did not create exactly one item";
			reset_active_probe();
			return active_probe_result::failed;
		}
		auto *item = find_inventory_item(
		    *inventory, value->item_instances.front().instance_id);
		if (!item || (item->m_flags & native_item_equipped) == 0)
		{
			error = "active native probe item was not equipped";
			reset_active_probe();
			return active_probe_result::failed;
		}

		const auto entity_id = value->entity_id;
		const auto handle = *m_probe_avatar;
		m_probe_avatar.reset();
		remove(handle);
		if (resolve_entity(entity_id))
		{
			error =
			    "active native probe entity survived forced removal";
			return active_probe_result::failed;
		}
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
		        "entity_system={} soul_list={}",
		        static_cast<void *>(context),
		        context
		            ? static_cast<void *>(context->m_pActorSystem)
		            : nullptr,
		        static_cast<void *>(environment),
		        environment
		            ? static_cast<void *>(environment->pEntitySystem)
		            : nullptr,
		        static_cast<void *>(
		            wh::rpgmodule::C_SoulList::GetInstance())));
		if (!context || !context->m_pActorSystem || !environment
		    || !environment->pEntitySystem
		    || !wh::rpgmodule::C_SoulList::GetInstance())
		{
			m_diagnostic =
			    "native ActorSystem, EntitySystem, or SoulList is unavailable";
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
		auto *context = wh::game::S_GameContext::GetInstance();
		const auto position = native_position(player.transform.position());
		const auto rotation = native_rotation(player.transform.rotation());
		const Vec3 scale(1.0F, 1.0F, 1.0F);
		const auto handle = m_next_handle++;
		const auto name = std::format(
		    "KCD2Online_Remote_{}_{}_{}",
		    m_epoch,
		    player.id,
		    handle);
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.engine-call.begin",
		    std::format(
		        "api=IActorSystem::CreateActor channel=0 name=\"{}\" "
		        "class=\"NPC\" template=<not-used-by-CreateActor> "
		        "soul=\"{}\" position=({:.6f},{:.6f},{:.6f}) "
		        "rotation=({:.6f},{:.6f},{:.6f},{:.6f}) "
		        "scale=(1,1,1) requested_entity_id=0 flags=0x{:08X} "
		        "actor_system={} entity_system={}",
		        name,
		        player.avatar.archetype_id(),
		        position.x,
		        position.y,
		        position.z,
		        rotation.v.x,
		        rotation.v.y,
		        rotation.v.z,
		        rotation.w,
		        remote_actor_creation_flags,
		        static_cast<void *>(context->m_pActorSystem),
		        SSystemGlobalEnvironment::GetInstance()
		                ? static_cast<void *>(
		                      SSystemGlobalEnvironment::GetInstance()
		                          ->pEntitySystem)
		                : nullptr));
		Offsets::IActor *actor_interface{};
		{
			auto spawn_scope =
			    m_entities.authorize_human_npc_spawn(name);
			actor_interface = guarded_create_actor(
			    context->m_pActorSystem,
			    name.c_str(),
			    &position,
			    &rotation,
			    &scale);
		}
		KCD2Online_JOIN_TRACE(
		    actor_interface
		        ? "join.remote-spawn.engine-call.returned"
		        : "join.remote-spawn.engine-call.nil",
		    std::format(
		        "api=IActorSystem::CreateActor actor={}",
		        static_cast<void *>(actor_interface)));
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.GetEntity.begin",
		    std::format(
		        "actor={}",
		        static_cast<void *>(actor_interface)));
		auto *entity =
		    actor_interface ? actor_interface->GetEntity() : nullptr;
		KCD2Online_JOIN_TRACE(
		    entity ? "join.remote-spawn.entity.resolved"
		           : "join.remote-spawn.entity.nil",
		    std::format(
		        "actor={} entity={}",
		        static_cast<void *>(actor_interface),
		        static_cast<void *>(entity)));
		if (!actor_interface || !entity)
		{
			m_diagnostic = "IActorSystem::CreateActor(NPC) failed";
			KCD2Online_JOIN_TRACE(
			    "join.remote-spawn.failed",
			    m_diagnostic);
			return std::nullopt;
		}

		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.GetId.begin",
		    std::format("entity={}", static_cast<void *>(entity)));
		const auto id = entity->GetId();
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.GetId.returned",
		    std::format("entity_id={}", id));
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.entity.configure.begin",
		    std::format(
		        "player_id={} handle={} entity_id={} entity={}",
		        player.id,
		        handle,
		        id,
		        static_cast<void *>(entity)));
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.register-player-entity.begin",
		    std::format("entity_id={}", id));
		m_entities.register_player_entity(id, player.id);
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.register-player-entity.returned",
		    std::format("entity_id={}", id));
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.GetFlags.begin",
		    std::format("entity_id={}", id));
		auto flags = entity->GetFlags();
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.GetFlags.returned",
		    std::format("entity_id={} flags=0x{:08X}", id, flags));
		flags &= ~(entity_flag_has_ai | entity_flag_trigger_areas
		    | entity_flag_no_proximity);
		flags |= remote_actor_creation_flags | entity_flag_calc_physics;
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.SetFlags.begin",
		    std::format("entity_id={} flags=0x{:08X}", id, flags));
		entity->SetFlags(flags);
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.SetFlags.returned",
		    std::format("entity_id={}", id));
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.SetAIObjectID.begin",
		    std::format("entity_id={} ai_object_id=0", id));
		entity->SetAIObjectID(0);
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.SetAIObjectID.returned",
		    std::format("entity_id={}", id));
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.EnablePhysics.begin",
		    std::format(
		        "entity_id={} entity={} enabled=false "
		        "api=fork:CEntity::EnablePhysics",
		        id,
		        static_cast<void *>(entity)));
		const auto physics_result =
		    reinterpret_cast<CEntity *>(entity)->EnablePhysics(false);
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.EnablePhysics.returned",
		    std::format(
		        "entity_id={} result={}",
		        id,
		        physics_result));
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.Hide.begin",
		    std::format("entity_id={} hidden=true", id));
		entity->Hide(true);
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.Hide.returned",
		    std::format("entity_id={}", id));
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.Activate.begin",
		    std::format("entity_id={} active=false", id));
		entity->Activate(false);
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.Activate.returned",
		    std::format("entity_id={}", id));
		KCD2Online_JOIN_TRACE(
		    "join.remote-spawn.entity.configure.complete",
		    std::format(
		        "entity_id={} flags=0x{:08X} client_only=true "
		        "never_network_static=true ai_object_id=0 "
		        "physics=false proximity=true hidden=true active=false "
		        "presentation=deferred",
		        id,
		        flags));

		const auto [iterator, inserted] = m_avatars.emplace(
		    handle,
		    entry{
		        .player = player.id,
		        .entity_id = id,
		        .epoch = m_epoch,
		        .shared_soul_guid = player.avatar.archetype_id()});
		(void)iterator;
		KCD2Online_JOIN_TRACE(
		    inserted ? "join.remote-spawn.success"
		             : "join.remote-spawn.handle-collision",
		    std::format(
		        "player_id={} handle={} entity_id={} actor={} entity={}",
		        player.id,
		        handle,
		        id,
		        static_cast<void *>(actor_interface),
		        static_cast<void *>(entity)));
		if (!inserted)
		{
			m_diagnostic = "remote avatar handle collision";
			m_entities.unregister_player_entity(id);
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			if (environment && environment->pEntitySystem)
				environment->pEntitySystem->RemoveEntity(id, true);
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
		if (!entity || !actor)
			return {
			    remote_avatar_state::failed,
			    "remote avatar entity was destroyed externally"};
		if (!actor->IsHumanActor())
			return {
			    remote_avatar_state::failed,
			    "remote avatar is not backed by a native Human"};
		// Retain only what replicated players need: mannequin/locomotion,
		// hit/condition handling and (below) Soul/Inventory.
		if (!actor->m_pMovementController || !actor->m_pMannequinStateParams
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
		if (!value->shared_soul_applied)
		{
			CryGUID guid{};
			auto *souls = wh::rpgmodule::C_SoulList::GetInstance();
			KCD2Online_JOIN_TRACE(
			    "join.remote-status.ApplySharedSoul.begin",
			    std::format(
			        "player_id={} entity_id={} soul={} soul_list={} "
			        "shared_soul_guid=\"{}\" api=fork:C_SoulList::ApplySharedSoul",
			        value->player,
			        value->entity_id,
			        static_cast<void *>(soul),
			        static_cast<void *>(souls),
			        value->shared_soul_guid));
			if (!souls
			    || !wh::ParseGuid(
			        value->shared_soul_guid.c_str(),
			        guid)
			    || !souls->ApplySharedSoul(*soul, guid))
			{
				value->failed = true;
				value->failure =
				    "native shared-Soul materialization failed";
				return {
				    remote_avatar_state::failed,
				    value->failure};
			}
			value->shared_soul_applied = true;
			value->shared_soul_applied_frame = m_frame_sequence;
			value->shared_soul_applied_at =
			    std::chrono::steady_clock::now();
			KCD2Online_JOIN_TRACE(
			    "join.remote-status.ApplySharedSoul.returned",
			    std::format(
			        "player_id={} entity_id={} result=true",
			        value->player,
			        value->entity_id));
			return {
			    remote_avatar_state::stabilizing_soul,
			    "waiting for native shared-Soul stabilization"};
		}
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
		    && value->presented
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
			auto corrected = player.transform;
			bool needs_write = true;
			if (entity && value->transform_applied)
			{
				const auto seconds = value->last_native_transform_at
			        == std::chrono::steady_clock::time_point{}
				    ? 1.0F / 60.0F
				    : std::chrono::duration<float>(
				          transform_started - value->last_native_transform_at)
				          .count();
				corrected = smooth_transform_target(
				    *entity,
				    player.transform,
				    seconds,
				    needs_write);
			}
			transform_succeeded = entity
			    && (!needs_write
			        || m_entities.write_transform(
			            entity,
			            corrected,
			            error));
			if (transform_succeeded)
				value->last_native_transform_at = transform_started;
			transform_time =
			    std::chrono::steady_clock::now() - transform_started;
		}
		bool motion_succeeded = true;
		if (transform_succeeded && value->presented && !activity_locked)
		{
			const auto motion_started = std::chrono::steady_clock::now();
			motion_succeeded = drive_motion(*value, player, error);
			motion_time = std::chrono::steady_clock::now() - motion_started;
		}
		if (transform_succeeded && value->presented)
			apply_animation(*value, player);
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
		// A freshly created Human is deliberately kept outside rendering,
		// physics and Actor updates while its shared Soul and inventory are
		// being replaced. The active ABI probe has always used this safe
		// lifecycle. Real remote players must not become tickable earlier than
		// the probe just because ServerAccepted already contains their snapshot.
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
			join_trace::write_diagnostic(
			    "performance.remote-avatar-update",
			    std::format(
			        "player_id={} handle={} total_ms={:.3f} "
			        "lifecycle_ms={:.3f} validation_ms={:.3f} "
			        "transform_changed={} transform_ms={:.3f} "
			        "motion_ms={:.3f} appearance_attempted={} "
			        "appearance_ms={:.3f}",
			        value->player,
			        avatar,
			        milliseconds(update_finished - update_started),
			        milliseconds(lifecycle_time),
			        milliseconds(validation_time),
			        transform_changed,
			        milliseconds(transform_time),
			        milliseconds(motion_time),
			        appearance_attempted,
			        milliseconds(appearance_time)));
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
		        "player_id={} entity_id={} physics=true hidden=false active=true",
		        avatar.player,
		        avatar.entity_id));
		if (!reinterpret_cast<CEntity *>(entity)->EnablePhysics(true))
		{
			error = "native remote physics could not be enabled";
			return false;
		}
		entity->Activate(true);
		entity->Hide(false);
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
		const auto station_guid = active
		    ? player.activity.station_guid()
		    : avatar.activity_station_guid;
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
		else if (avatar.activity_kind
		    == protocol::PLAYER_ACTIVITY_KIND_BLACKSMITHING)
		{
			action = "SmithHarden";
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

		if (active)
		{
			avatar.activity_active = true;
			avatar.activity_kind = player.activity.kind();
			avatar.activity_session_id = player.activity.session_id();
			avatar.activity_station_guid = player.activity.station_guid();
		}
		else
		{
			avatar.activity_active = false;
			avatar.activity_kind = protocol::PLAYER_ACTIVITY_KIND_NONE;
			avatar.activity_session_id = 0;
			avatar.activity_station_guid = 0;
			avatar.motion_applied = false;
			avatar.transform_applied = false;
		}
		return true;
	}

	void native_remote_avatar_backend::remove(
	    remote_avatar_handle avatar)
	{
		const auto found = m_avatars.find(avatar);
		if (found == m_avatars.end())
			return;
		const auto id = found->second.entity_id;
		std::string ignored;
		(void)remove_created_items(found->second, ignored);
		m_entities.unregister_player_entity(id);
		auto *environment = SSystemGlobalEnvironment::GetInstance();
		if (environment && environment->pEntitySystem
		    && environment->pEntitySystem->GetEntity(id))
			environment->pEntitySystem->RemoveEntity(id, true);
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
		if (m_native_weapon_actions_enabled
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
				m_native_weapon_actions_enabled = false;
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

	bool native_remote_avatar_backend::drive_motion(
	    entry &avatar,
	    const remote_avatar_snapshot &player,
	    std::string &error)
	{
		const auto now = std::chrono::steady_clock::now();
		const auto &velocity = player.transform.velocity();
		const bool motion_changed = !avatar.motion_applied
		    || avatar.last_movement_mode != player.movement_mode
		    || velocity_changed(avatar.last_motion_velocity, velocity)
		    || (player.transform.has_locomotion()
		        && locomotion_changed(
		            avatar.last_locomotion,
		            player.transform.locomotion()));
		const bool keepalive_due = avatar.motion_applied
		    && player.movement_mode != protocol::MOVEMENT_MODE_IDLE
		    && now - avatar.last_motion_request_at
		        >= remote_motion_keepalive;
		if (!motion_changed && !keepalive_due)
			return true;

		auto *actor = resolve_actor(avatar.entity_id);
		auto *controller =
		    actor ? actor->m_pMovementController : nullptr;
		if (!avatar.first_motion_logged)
		{
			avatar.first_motion_logged = true;
			KCD2Online_JOIN_TRACE(
			    "join.remote-animation.first-locomotion",
			    std::format(
			        "player_id={} entity_id={} actor={} controller={} "
			        "movement_mode={} api=C_Actor::RequestLocomotion",
			        avatar.player,
			        avatar.entity_id,
			        static_cast<void *>(actor),
			        static_cast<void *>(controller),
			        static_cast<int>(player.movement_mode)));
		}
		if (!actor || !controller)
		{
			// Controller construction is part of the asynchronous readiness
			// chain; transform interpolation still applies in the meantime.
			if (avatar.lifecycle_ready)
			{
				error = actor
				    ? "native remote MovementController disappeared"
				    : "native remote actor disappeared";
				return false;
			}
			return true;
		}
		float speed{};
		if (player.transform.has_locomotion())
			speed = player.transform.locomotion().speed();
		else switch (player.movement_mode)
		{
		case protocol::MOVEMENT_MODE_WALK:
			speed = 1.5F;
			break;
		case protocol::MOVEMENT_MODE_RUN:
			speed = 3.8F;
			break;
		case protocol::MOVEMENT_MODE_SPRINT:
			speed = 5.0F;
			break;
		case protocol::MOVEMENT_MODE_IDLE:
		default:
			break;
		}
		std::optional<Vec3> move_target;
		std::optional<Vec3> facing_direction;
		if (speed > 0.0F)
		{
			Vec3 direction(velocity.x(), velocity.y(), velocity.z());
			const auto length = direction.GetLength();
			if (length > 0.001F)
				direction /= length;
			else
			{
				auto *entity = resolve_entity(avatar.entity_id);
				if (entity)
					entity->GetForwardDir(direction);
			}
			move_target =
			    native_position(player.transform.position())
			    + direction * std::clamp(speed * 0.4F, 1.2F, 2.5F);
		}
		if (player.transform.has_locomotion()
		    && player.transform.locomotion().has_facing_direction())
		{
			facing_direction = native_position(
			    player.transform.locomotion().facing_direction());
		}
		if (m_native_locomotion_enabled)
		{
			const SMultiplayerLocomotionRequest request{
			    move_target ? &*move_target : nullptr,
			    facing_direction ? &*facing_direction : nullptr,
			    speed,
			    player.transform.has_locomotion()
			        && player.transform.locomotion().strafing()};
			const auto result = guarded_request_locomotion(
			    *actor,
			    request);
			if (result != locomotion_request_result::applied)
			{
				// RequestMovement is an optional presentation enhancement. Some
				// native NPC controllers reject player-style requests (and older
				// layouts may fault). Transform replication remains authoritative,
				// so disable this ABI path for the process instead of failing the
				// remote avatar and unloading the multiplayer world.
				m_native_locomotion_enabled = false;
				KCD2Online_JOIN_TRACE(
				    "join.remote-animation.locomotion-disabled",
				    std::format(
				        "player_id={} entity_id={} speed={} reason={}",
				        avatar.player,
				        avatar.entity_id,
				        speed,
				        result == locomotion_request_result::faulted
				            ? "seh"
				            : "rejected"));
			}
		}
		avatar.motion_applied = true;
		avatar.last_movement_mode = player.movement_mode;
		avatar.last_motion_velocity = velocity;
		if (player.transform.has_locomotion())
			avatar.last_locomotion = player.transform.locomotion();
		avatar.last_motion_request_at = now;
		return true;
	}

	void native_remote_avatar_backend::apply_animation(
	    entry &avatar,
	    const remote_avatar_snapshot &player)
	{
		if (!m_native_animation_actions_enabled
		    || !player.transform.has_animation())
			return;
		const auto &animation = player.transform.animation();
		if (animation.sequence() <= avatar.last_animation_sequence)
			return;
		avatar.last_animation_sequence = animation.sequence();
		if (avatar.activity_active)
			return;

		const auto script = animation.active()
		    ? std::format(
		          "local e=System.GetEntity({}) if e and e.human "
		          "and e.human.PlayAnim then e.human:PlayAnim({},'') end",
		          avatar.entity_id,
		          lua_string(animation.fragment()))
		    : std::format(
		          "local e=System.GetEntity({}) if e and e.human "
		          "and e.human.StopAnim then e.human:StopAnim() end",
		          avatar.entity_id);
		if (!execute_remote_script(script))
		{
			m_native_animation_actions_enabled = false;
			KCD2Online_JOIN_TRACE(
			    "join.remote-animation.mannequin-disabled",
			    std::format(
			        "player_id={} entity_id={} sequence={} fragment=\"{}\"",
			        avatar.player,
			        avatar.entity_id,
			        animation.sequence(),
			        animation.fragment()));
			return;
		}
		KCD2Online_JOIN_TRACE(
		    "join.remote-animation.mannequin-applied",
		    std::format(
		        "player_id={} entity_id={} sequence={} active={} fragment=\"{}\" api=C_ScriptBindHuman::{}",
		        avatar.player,
		        avatar.entity_id,
		        animation.sequence(),
		        animation.active(),
		        animation.fragment(),
		        animation.active() ? "PlayAnim" : "StopAnim"));
	}
}
