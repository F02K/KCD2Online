#pragma once

#include <cstdint>
#include <source_location>
#include <string_view>

#ifdef _WIN32
#include <Windows.h>
#include <excpt.h>
#endif

namespace kcd2o::kcse::join_trace
{
	enum class thread_role
	{
		unknown,
		abi,
		network,
		kcse_post_update
	};

	void set_thread_role(thread_role role) noexcept;
	[[nodiscard]] thread_role current_thread_role() noexcept;
	[[nodiscard]] const char *thread_role_name(thread_role role) noexcept;

	[[nodiscard]] std::uint64_t begin_join(
	    std::string_view server_target,
	    std::source_location location = std::source_location::current()) noexcept;
	void finish_join(
	    std::string_view outcome,
	    std::source_location location = std::source_location::current()) noexcept;
	[[nodiscard]] bool active() noexcept;
	[[nodiscard]] std::uint64_t session_id() noexcept;
	void set_diagnostics_enabled(bool enabled) noexcept;
	[[nodiscard]] bool diagnostics_enabled() noexcept;

	void write(
	    std::string_view event,
	    std::string_view detail = {},
	    std::source_location location = std::source_location::current()) noexcept;
	void write_diagnostic(
	    std::string_view event,
	    std::string_view detail = {},
	    std::source_location location = std::source_location::current()) noexcept;

#ifdef _WIN32
	long seh_filter(
	    EXCEPTION_POINTERS *exception,
	    std::string_view event,
	    std::source_location location = std::source_location::current()) noexcept;
#endif
}

#define KCD2Online_JOIN_TRACE(event, detail)                                     \
	do                                                                       \
	{                                                                        \
		if (::kcd2o::kcse::join_trace::active()                              \
		    && ::kcd2o::kcse::join_trace::diagnostics_enabled())             \
		{                                                                      \
			::kcd2o::kcse::join_trace::write(                                  \
			    (event),                                                        \
			    (detail),                                                       \
			    std::source_location::current());                               \
		}                                                                      \
	} while (false)

#ifdef _WIN32
#define KCD2Online_JOIN_SEH_FILTER(event)                                        \
	::kcd2o::kcse::join_trace::seh_filter(                                  \
	    GetExceptionInformation(),                                           \
	    (event),                                                             \
	    std::source_location::current())
#endif
