#pragma once

#include <cstddef>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace kcd2o
{
	using controlled_entity = void *;

	struct controlled_entity_state
	{
		bool active{};
		bool hidden{};
	};

	struct entity_control_result
	{
		bool changed{};
		std::size_t affected{};
		std::size_t restored{};
		std::size_t failed{};
	};

	class entity_control_backend
	{
	public:
		virtual ~entity_control_backend() = default;
		[[nodiscard]] virtual bool should_disable(
		    controlled_entity entity) const = 0;
		[[nodiscard]] virtual bool is_active(controlled_entity entity) const = 0;
		[[nodiscard]] virtual bool is_hidden(controlled_entity entity) const = 0;
		[[nodiscard]] virtual bool set_active(
		    controlled_entity entity,
		    bool active) = 0;
		[[nodiscard]] virtual bool set_hidden(
		    controlled_entity entity,
		    bool hidden) = 0;
	};

	class entity_controller
	{
	public:
		explicit entity_controller(entity_control_backend &backend) :
		    m_backend(backend)
		{
		}

		[[nodiscard]] entity_control_result set_disabled(
		    bool disabled,
		    std::span<const controlled_entity> entities = {})
		{
			entity_control_result result;
			if (m_disabled == disabled)
			{
				return result;
			}
			result.changed = true;
			m_disabled = disabled;
			if (disabled)
			{
				for (const auto entity : entities)
				{
					const auto applied = disable(entity);
					result.affected += applied.affected;
					result.failed += applied.failed;
				}
				return result;
			}

			for (auto iterator = m_original_states.begin();
			     iterator != m_original_states.end();)
			{
				if (restore(iterator->first, iterator->second))
				{
					++result.restored;
				}
				else
				{
					++result.failed;
				}
				iterator = m_original_states.erase(iterator);
			}
			return result;
		}

		[[nodiscard]] entity_control_result entity_created(
		    controlled_entity entity)
		{
			return m_disabled ? disable(entity) : entity_control_result{};
		}

		void entity_destroyed(controlled_entity entity)
		{
			m_original_states.erase(entity);
			m_player_entities.erase(entity);
		}

		[[nodiscard]] entity_control_result register_player(
		    controlled_entity entity)
		{
			entity_control_result result;
			if (!entity)
			{
				return result;
			}
			m_player_entities.insert(entity);
			const auto iterator = m_original_states.find(entity);
			if (iterator == m_original_states.end())
			{
				return result;
			}
			result.changed = true;
			if (restore(entity, iterator->second))
			{
				result.restored = 1;
			}
			else
			{
				result.failed = 1;
			}
			m_original_states.erase(iterator);
			return result;
		}

		void unregister_player(controlled_entity entity)
		{
			m_player_entities.erase(entity);
		}

		[[nodiscard]] bool is_player(controlled_entity entity) const
		{
			return entity && m_player_entities.contains(entity);
		}

		[[nodiscard]] bool disabled() const
		{
			return m_disabled;
		}

		[[nodiscard]] std::size_t controlled_count() const
		{
			return m_original_states.size();
		}

	private:
		[[nodiscard]] entity_control_result disable(controlled_entity entity)
		{
			entity_control_result result;
			if (!entity || is_player(entity)
			    || m_original_states.contains(entity)
			    || !m_backend.should_disable(entity))
			{
				return result;
			}
			const controlled_entity_state original{
			    m_backend.is_active(entity),
			    m_backend.is_hidden(entity)};
			if (!m_backend.set_hidden(entity, true)
			    || !m_backend.set_active(entity, false))
			{
				(void)restore(entity, original);
				result.failed = 1;
				return result;
			}
			m_original_states.emplace(entity, original);
			result.affected = 1;
			return result;
		}

		[[nodiscard]] bool restore(
		    controlled_entity entity,
		    const controlled_entity_state &state)
		{
			const bool hidden = m_backend.set_hidden(entity, state.hidden);
			const bool active = m_backend.set_active(entity, state.active);
			return hidden && active;
		}

		entity_control_backend &m_backend;
		bool m_disabled{};
		std::unordered_set<controlled_entity> m_player_entities;
		std::unordered_map<controlled_entity, controlled_entity_state>
		    m_original_states;
	};
}
