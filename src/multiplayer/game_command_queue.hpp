#pragma once

#include "multiplayer/protocol.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

namespace kcd2o
{
	class game_command_queue
	{
	public:
		explicit game_command_queue(std::size_t capacity = 4096) :
		    m_capacity(capacity)
		{
		}

		[[nodiscard]] bool push(
		    protocol::Envelope envelope,
		    bool reliable)
		{
			std::scoped_lock lock(m_mutex);
			if (envelope.has_world_snapshot())
			{
				for (auto iterator = m_queue.rbegin();
				     iterator != m_queue.rend();
				     ++iterator)
				{
					if (iterator->has_world_snapshot())
					{
						*iterator = std::move(envelope);
						return true;
					}
				}
			}
			if (m_queue.size() >= m_capacity)
			{
				if (!reliable)
				{
					return false;
				}
				for (auto iterator = m_queue.begin();
				     iterator != m_queue.end();
				     ++iterator)
				{
					if (iterator->has_world_snapshot())
					{
						m_queue.erase(iterator);
						m_queue.push_back(std::move(envelope));
						return true;
					}
				}
				return false;
			}
			m_queue.push_back(std::move(envelope));
			return true;
		}

		[[nodiscard]] std::vector<protocol::Envelope> drain(
		    std::size_t maximum = 128)
		{
			std::scoped_lock lock(m_mutex);
			const auto count = std::min(maximum, m_queue.size());
			std::vector<protocol::Envelope> result;
			result.reserve(count);
			for (std::size_t index = 0; index < count; ++index)
			{
				result.push_back(std::move(m_queue.front()));
				m_queue.pop_front();
			}
			return result;
		}

		[[nodiscard]] std::size_t size() const
		{
			std::scoped_lock lock(m_mutex);
			return m_queue.size();
		}

		void clear()
		{
			std::scoped_lock lock(m_mutex);
			m_queue.clear();
		}

	private:
		std::size_t m_capacity;
		mutable std::mutex m_mutex;
		std::deque<protocol::Envelope> m_queue;
	};
}
