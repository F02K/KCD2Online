#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace kcd2o::npc
{
	using owner_id = std::uint64_t;
	using native_handle = std::uintptr_t;

	struct handle
	{
		std::uint32_t slot{};
		std::uint32_t generation{};

		[[nodiscard]] explicit operator bool() const
		{
			return generation != 0;
		}

		friend bool operator==(const handle &, const handle &) = default;
	};

	enum class locomotion
	{
		idle,
		walk,
		run
	};

	enum class stance
	{
		relaxed,
		ready
	};

	enum class weapon_class
	{
		none,
		unarmed,
		one_handed,
		two_handed,
		polearm,
		bow,
		crossbow
	};

	enum class state
	{
		pending,
		ready,
		failed,
		removed
	};

	enum class error_code
	{
		none,
		unavailable,
		invalid_request,
		capacity,
		invalid_handle,
		spawn_failed,
		update_failed,
		timed_out,
		externally_destroyed
	};

	struct transform
	{
		std::array<float, 3> position{};
		std::array<float, 4> rotation{0.0F, 0.0F, 0.0F, 1.0F};
	};

	struct motion
	{
		locomotion mode{locomotion::idle};
		std::array<float, 3> velocity{};

		friend bool operator==(const motion &, const motion &) = default;
	};

	struct equipment
	{
		std::string definition_id;
		std::string equipped_slot;

		friend bool operator==(const equipment &, const equipment &) = default;
	};

	struct appearance
	{
		std::vector<equipment> items;
		stance pose{stance::relaxed};
		weapon_class weapon{weapon_class::none};
		bool weapon_drawn{};

		friend bool operator==(const appearance &, const appearance &) = default;
	};

	struct spawn_request
	{
		std::string diagnostic_context;
		std::string archetype_id;
		transform world_transform;
		locomotion movement{locomotion::idle};
		std::array<float, 3> velocity{};
		appearance visual;
		bool exempt_from_entity_control{};
	};

	struct capability
	{
		bool available{};
		std::string diagnostic;
	};

	struct status
	{
		state value{state::removed};
		error_code error{error_code::none};
		std::string diagnostic;
	};

	struct spawn_result
	{
		handle npc;
		error_code error{error_code::none};
		std::string diagnostic;

		[[nodiscard]] explicit operator bool() const
		{
			return static_cast<bool>(npc);
		}
	};

	class backend
	{
	public:
		virtual ~backend() = default;
		[[nodiscard]] virtual capability get_capability() const = 0;
		[[nodiscard]] virtual std::optional<native_handle> spawn(
		    const spawn_request &request,
		    std::string &error) = 0;
		[[nodiscard]] virtual status poll(native_handle npc) = 0;
		[[nodiscard]] virtual bool set_transform(
		    native_handle npc,
		    const transform &value) = 0;
		[[nodiscard]] virtual bool set_motion(
		    native_handle npc,
		    const motion &value) = 0;
		[[nodiscard]] virtual bool set_appearance(
		    native_handle npc,
		    const appearance &value) = 0;
		virtual void remove(native_handle npc) = 0;
	};

	class manager
	{
	public:
		using clock = std::chrono::steady_clock;

		explicit manager(
		    backend &implementation,
		    std::size_t capacity = 256,
		    std::chrono::seconds spawn_timeout =
		        std::chrono::seconds(10)) :
		    m_backend(implementation),
		    m_slots(capacity),
		    m_spawn_timeout(spawn_timeout)
		{
		}

		[[nodiscard]] capability get_capability() const
		{
			return m_backend.get_capability();
		}

		[[nodiscard]] spawn_result spawn(
		    spawn_request request,
		    owner_id owner,
		    clock::time_point now = clock::now())
		{
			if (owner == 0 || !valid_request(request))
			{
				return {
				    {},
				    error_code::invalid_request,
				    "NPC spawn request is invalid"};
			}
			const auto available = m_backend.get_capability();
			if (!available.available)
			{
				return {
				    {},
				    error_code::unavailable,
				    available.diagnostic};
			}

			std::scoped_lock lock(m_mutex);
			for (std::size_t index = 0; index < m_slots.size(); ++index)
			{
				auto &slot = m_slots[index];
				if (slot.allocated)
				{
					continue;
				}
				slot.allocated = true;
				slot.owner = owner;
				slot.current_state = state::pending;
				slot.error = error_code::none;
				slot.diagnostic.clear();
				slot.request = std::move(request);
				slot.desired_transform = slot.request.world_transform;
				slot.desired_motion = {
				    slot.request.movement,
				    slot.request.velocity};
				slot.desired_appearance = slot.request.visual;
				slot.transform_dirty = true;
				slot.motion_dirty = true;
				slot.appearance_dirty = true;
				slot.remove_requested = false;
				slot.native = 0;
				slot.deadline = now + m_spawn_timeout;
				if (slot.generation == 0)
				{
					slot.generation = 1;
				}
				return {
				    {static_cast<std::uint32_t>(index), slot.generation},
				    error_code::none,
				    {}};
			}
			return {
			    {},
			    error_code::capacity,
			    "NPC manager capacity is exhausted"};
		}

		[[nodiscard]] status get_status(handle npc) const
		{
			std::scoped_lock lock(m_mutex);
			const auto *slot = find(npc);
			if (!slot)
			{
				return {
				    state::removed,
				    error_code::invalid_handle,
				    "NPC handle is stale or invalid"};
			}
			return {
			    slot->current_state,
			    slot->error,
			    slot->diagnostic};
		}

		[[nodiscard]] bool set_transform(
		    handle npc,
		    transform value)
		{
			if (!valid_transform(value))
				return false;
			std::scoped_lock lock(m_mutex);
			auto *slot = find(npc);
			if (!slot || slot->remove_requested)
				return false;
			slot->desired_transform = std::move(value);
			slot->transform_dirty = true;
			return true;
		}

		[[nodiscard]] bool set_locomotion(
		    handle npc,
		    locomotion value)
		{
			std::scoped_lock lock(m_mutex);
			auto *slot = find(npc);
			if (!slot || slot->remove_requested)
				return false;
			slot->desired_motion.mode = value;
			slot->desired_motion.velocity = {};
			slot->motion_dirty = true;
			return true;
		}

		[[nodiscard]] bool set_motion(
		    handle npc,
		    motion value)
		{
			if (!valid_motion(value))
				return false;
			std::scoped_lock lock(m_mutex);
			auto *slot = find(npc);
			if (!slot || slot->remove_requested)
				return false;
			slot->desired_motion = value;
			slot->motion_dirty = true;
			return true;
		}

		[[nodiscard]] bool set_appearance(
		    handle npc,
		    appearance value)
		{
			if (!valid_appearance(value))
			{
				return false;
			}
			std::scoped_lock lock(m_mutex);
			auto *slot = find(npc);
			if (!slot || slot->remove_requested)
				return false;
			slot->desired_appearance = std::move(value);
			slot->appearance_dirty = true;
			return true;
		}

		[[nodiscard]] bool remove(handle npc)
		{
			std::scoped_lock lock(m_mutex);
			auto *slot = find(npc);
			if (!slot)
				return false;
			slot->remove_requested = true;
			return true;
		}

		std::size_t clear_owner(owner_id owner)
		{
			std::scoped_lock lock(m_mutex);
			std::size_t count{};
			for (auto &slot : m_slots)
			{
				if (slot.allocated && slot.owner == owner
				    && !slot.remove_requested)
				{
					slot.remove_requested = true;
					++count;
				}
			}
			return count;
		}

		void native_destroyed(native_handle native)
		{
			if (!native)
				return;
			std::scoped_lock lock(m_mutex);
			for (auto &slot : m_slots)
			{
				if (slot.allocated && slot.native == native)
				{
					slot.native = 0;
					fail(
					    slot,
					    error_code::externally_destroyed,
					    "NPC entity was destroyed outside the manager");
					return;
				}
			}
		}

		void tick(clock::time_point now = clock::now())
		{
			std::scoped_lock lock(m_mutex);
			for (auto &slot : m_slots)
			{
				if (!slot.allocated)
					continue;
				if (slot.remove_requested)
				{
					release(slot);
					continue;
				}
				if (slot.current_state == state::failed)
					continue;
				if (!slot.native)
				{
					std::string error;
					slot.native = m_backend.spawn(slot.request, error).value_or(0);
					if (!slot.native)
					{
						fail(
						    slot,
						    error_code::spawn_failed,
						    error.empty() ? "native NPC spawn failed" : error);
						continue;
					}
				}
				const auto backend_status = m_backend.poll(slot.native);
				if (backend_status.value == state::failed)
				{
					m_backend.remove(slot.native);
					slot.native = 0;
					fail(
					    slot,
					    backend_status.error == error_code::none
					        ? error_code::spawn_failed
					        : backend_status.error,
					    backend_status.diagnostic);
					continue;
				}
				if (backend_status.value != state::ready)
				{
					if (now >= slot.deadline)
					{
						m_backend.remove(slot.native);
						slot.native = 0;
						fail(
						    slot,
						    error_code::timed_out,
						    "native NPC spawn timed out");
					}
					continue;
				}
				slot.current_state = state::ready;
				if (!apply_updates(slot))
				{
					m_backend.remove(slot.native);
					slot.native = 0;
					fail(
					    slot,
					    error_code::update_failed,
					    "native NPC state update failed");
				}
			}
		}

	private:
		[[nodiscard]] static bool valid_transform(
		    const transform &value)
		{
			for (const auto component : value.position)
			{
				if (!std::isfinite(component))
					return false;
			}
			float rotation_length{};
			for (const auto component : value.rotation)
			{
				if (!std::isfinite(component))
					return false;
				rotation_length += component * component;
			}
			return rotation_length > 0.000001F;
		}

		[[nodiscard]] static bool valid_appearance(
		    const appearance &value)
		{
			if (value.items.size() > 32
			    || (value.weapon_drawn
			        && value.weapon == weapon_class::none))
				return false;
			std::unordered_set<std::string> slots;
			for (const auto &item : value.items)
			{
				if (item.definition_id.empty()
				    || item.equipped_slot.empty()
				    || !slots.insert(item.equipped_slot).second)
					return false;
			}
			return true;
		}

		[[nodiscard]] static bool valid_motion(const motion &value)
		{
			return std::ranges::all_of(
			    value.velocity,
			    [](float component)
			    {
				    return std::isfinite(component);
			    });
		}

		[[nodiscard]] static bool valid_request(
		    const spawn_request &value)
		{
			return !value.archetype_id.empty()
			    && valid_transform(value.world_transform)
			    && valid_motion({value.movement, value.velocity})
			    && valid_appearance(value.visual);
		}

		struct slot
		{
			bool allocated{};
			std::uint32_t generation{};
			owner_id owner{};
			state current_state{state::removed};
			error_code error{error_code::none};
			std::string diagnostic;
			spawn_request request;
			transform desired_transform;
			motion desired_motion;
			appearance desired_appearance;
			bool transform_dirty{};
			bool motion_dirty{};
			bool appearance_dirty{};
			bool remove_requested{};
			native_handle native{};
			clock::time_point deadline;
		};

		[[nodiscard]] slot *find(handle npc)
		{
			if (!npc || npc.slot >= m_slots.size())
				return nullptr;
			auto &slot = m_slots[npc.slot];
			return slot.allocated && slot.generation == npc.generation
			    ? &slot
			    : nullptr;
		}

		[[nodiscard]] const slot *find(handle npc) const
		{
			if (!npc || npc.slot >= m_slots.size())
				return nullptr;
			const auto &slot = m_slots[npc.slot];
			return slot.allocated && slot.generation == npc.generation
			    ? &slot
			    : nullptr;
		}

		bool apply_updates(slot &value)
		{
			if (value.transform_dirty
			    && !m_backend.set_transform(
			        value.native,
			        value.desired_transform))
				return false;
			if (value.motion_dirty
			    && !m_backend.set_motion(
			        value.native,
			        value.desired_motion))
				return false;
			if (value.appearance_dirty
			    && !m_backend.set_appearance(
			        value.native,
			        value.desired_appearance))
				return false;
			value.transform_dirty = false;
			value.motion_dirty = false;
			value.appearance_dirty = false;
			return true;
		}

		void fail(
		    slot &value,
		    error_code error,
		    std::string diagnostic)
		{
			value.current_state = state::failed;
			value.error = error;
			value.diagnostic = std::move(diagnostic);
		}

		void release(slot &value)
		{
			if (value.native)
			{
				m_backend.remove(value.native);
			}
			const auto next_generation =
			    value.generation == UINT32_MAX
			    ? 1U
			    : value.generation + 1U;
			value = {};
			value.generation = next_generation;
		}

		backend &m_backend;
		mutable std::mutex m_mutex;
		std::vector<slot> m_slots;
		std::chrono::seconds m_spawn_timeout;
	};

	[[nodiscard]] inline std::string_view to_string(state value)
	{
		switch (value)
		{
		case state::pending:
			return "pending";
		case state::ready:
			return "ready";
		case state::failed:
			return "failed";
		case state::removed:
			return "removed";
		}
		return "unknown";
	}
}
