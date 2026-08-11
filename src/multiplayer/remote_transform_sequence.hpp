#pragma once

#include <cstdint>

namespace kcd2o
{
	class remote_transform_sequence
	{
	public:
		[[nodiscard]] constexpr bool accept(
		    std::uint64_t sequence) noexcept
		{
			if (m_initialized && sequence <= m_value)
				return false;
			m_initialized = true;
			m_value = sequence;
			return true;
		}

		constexpr void reset() noexcept
		{
			m_initialized = false;
			m_value = 0;
		}

		[[nodiscard]] constexpr bool initialized() const noexcept
		{
			return m_initialized;
		}

		[[nodiscard]] constexpr std::uint64_t value() const noexcept
		{
			return m_value;
		}

	private:
		bool m_initialized{};
		std::uint64_t m_value{};
	};
}
