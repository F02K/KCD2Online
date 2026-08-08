#pragma once

#include <chrono>
#include <cstdint>

namespace kcd2o::kcse
{
	inline constexpr std::uint64_t remote_soul_settle_frames = 3;
	inline constexpr auto remote_soul_settle_time =
	    std::chrono::milliseconds{250};

	struct remote_soul_settle_status
	{
		std::uint64_t elapsed_frames{};
		std::chrono::milliseconds elapsed_time{};
		bool ready{};
	};

	[[nodiscard]] inline remote_soul_settle_status
	evaluate_remote_soul_settle(
	    std::uint64_t current_frame,
	    std::uint64_t applied_frame,
	    std::chrono::steady_clock::time_point now,
	    std::chrono::steady_clock::time_point applied_at)
	{
		const auto frames = current_frame >= applied_frame
		    ? current_frame - applied_frame
		    : 0;
		const auto elapsed = now >= applied_at
		    ? std::chrono::duration_cast<std::chrono::milliseconds>(
	          now - applied_at)
		    : std::chrono::milliseconds{};
		return {
		    frames,
		    elapsed,
		    frames >= remote_soul_settle_frames
		        && elapsed >= remote_soul_settle_time};
	}
}
