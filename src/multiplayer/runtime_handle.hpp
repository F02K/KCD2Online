#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace kcd2o
{
	struct runtime_handle
	{
		std::uint32_t slot{};
		std::uint32_t generation{};
		std::uint64_t epoch{};

		[[nodiscard]] bool operator==(const runtime_handle &) const = default;
	};

	class runtime_handle_registry
	{
	public:
		explicit runtime_handle_registry(std::uint64_t epoch = 1) :
		    m_epoch(epoch == 0 ? 1 : epoch)
		{
		}

		[[nodiscard]] runtime_handle allocate()
		{
			for (std::uint32_t slot = 0; slot < m_entries.size(); ++slot)
			{
				auto &entry = m_entries[slot];
				if (!entry.active)
				{
					entry.active = true;
					return {slot, entry.generation, m_epoch};
				}
			}
			m_entries.push_back({1, true});
			return {
			    static_cast<std::uint32_t>(m_entries.size() - 1),
			    1,
			    m_epoch};
		}

		[[nodiscard]] bool valid(runtime_handle handle) const
		{
			return handle.epoch == m_epoch
			    && handle.slot < m_entries.size()
			    && m_entries[handle.slot].active
			    && m_entries[handle.slot].generation == handle.generation;
		}

		[[nodiscard]] bool release(runtime_handle handle)
		{
			if (!valid(handle))
				return false;
			auto &entry = m_entries[handle.slot];
			entry.active = false;
			++entry.generation;
			if (entry.generation == 0)
				entry.generation = 1;
			return true;
		}

		void reset_epoch(std::uint64_t epoch)
		{
			m_epoch = epoch == 0 ? m_epoch + 1 : epoch;
			if (m_epoch == 0)
				m_epoch = 1;
			for (auto &entry : m_entries)
			{
				entry.active = false;
				++entry.generation;
				if (entry.generation == 0)
					entry.generation = 1;
			}
		}

		[[nodiscard]] std::uint64_t epoch() const noexcept
		{
			return m_epoch;
		}

	private:
		struct entry
		{
			std::uint32_t generation{1};
			bool active{};
		};

		std::uint64_t m_epoch;
		std::vector<entry> m_entries;
	};
}
