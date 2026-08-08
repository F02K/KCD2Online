#include "kcse/join_trace.hpp"

#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>

namespace kcd2o::kcse::join_trace
{
	namespace
	{
		std::atomic_bool g_active{};
		std::atomic_bool g_diagnostics_enabled{};
		std::atomic_uint64_t g_session{};
		std::mutex g_write_mutex;
		HANDLE g_trace_file{INVALID_HANDLE_VALUE};
		std::string g_pending_lines;
		ULONGLONG g_last_write_tick{};
		thread_local thread_role g_thread_role{thread_role::unknown};
		constexpr std::size_t trace_buffer_capacity = 32U * 1024U;
		constexpr ULONGLONG trace_write_interval_ms = 250;

		std::filesystem::path trace_path()
		{
			HMODULE module{};
			const auto address = reinterpret_cast<LPCWSTR>(
			    reinterpret_cast<std::uintptr_t>(&trace_path));
			if (GetModuleHandleExW(
			        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
			            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			        address,
			        &module))
			{
				std::wstring path(32768, L'\0');
				const auto length = GetModuleFileNameW(
				    module,
				    path.data(),
				    static_cast<DWORD>(path.size()));
				if (length != 0 && length < path.size())
				{
					path.resize(length);
					return std::filesystem::path(path).parent_path()
					    / L"KCD2Online-join.log";
				}
			}
			return std::filesystem::current_path() / L"KCD2Online-join.log";
		}

		std::string_view filename(std::string_view path)
		{
			const auto slash = path.find_last_of("/\\");
			return slash == std::string_view::npos
			    ? path
			    : path.substr(slash + 1);
		}

		bool open_trace_file() noexcept
		{
			if (g_trace_file != INVALID_HANDLE_VALUE)
				return true;
			try
			{
				const auto path = trace_path();
				g_trace_file = CreateFileW(
				    path.c_str(),
				    FILE_APPEND_DATA,
				    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				    nullptr,
				    OPEN_ALWAYS,
				    FILE_ATTRIBUTE_NORMAL,
				    nullptr);
			}
			catch (...)
			{
				g_trace_file = INVALID_HANDLE_VALUE;
			}
			return g_trace_file != INVALID_HANDLE_VALUE;
		}

		void flush_pending_locked(bool durable) noexcept
		{
			if (g_pending_lines.empty())
			{
				if (durable && g_trace_file != INVALID_HANDLE_VALUE)
					FlushFileBuffers(g_trace_file);
				return;
			}
			if (!open_trace_file())
			{
				OutputDebugStringA(g_pending_lines.c_str());
				g_pending_lines.clear();
				return;
			}
			DWORD written{};
			WriteFile(
			    g_trace_file,
			    g_pending_lines.data(),
			    static_cast<DWORD>(g_pending_lines.size()),
			    &written,
			    nullptr);
			g_pending_lines.clear();
			g_last_write_tick = GetTickCount64();
			if (durable)
				FlushFileBuffers(g_trace_file);
		}

		void append(std::string_view line) noexcept
		{
			try
			{
				std::scoped_lock lock(g_write_mutex);
				g_pending_lines.append(line);
				const auto now = GetTickCount64();
				if (g_pending_lines.size() >= trace_buffer_capacity
				    || g_last_write_tick == 0
				    || now - g_last_write_tick >= trace_write_interval_ms)
					flush_pending_locked(false);
			}
			catch (...)
			{
				// Diagnostics must never become a second crash source.
			}
		}

		void flush(bool durable) noexcept
		{
			try
			{
				std::scoped_lock lock(g_write_mutex);
				flush_pending_locked(durable);
			}
			catch (...)
			{
				// Diagnostics must never become a second crash source.
			}
		}

		void write_line(
		    std::string_view event,
		    std::string_view detail,
		    std::source_location location) noexcept
		{
			try
			{
				SYSTEMTIME time{};
				GetLocalTime(&time);
				const auto line = std::format(
				    "[{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}] "
				    "[join={}] [pid={}] [tid={} role={}] [{}:{} {}] {}{}{}\r\n",
				    time.wYear,
				    time.wMonth,
				    time.wDay,
				    time.wHour,
				    time.wMinute,
				    time.wSecond,
				    time.wMilliseconds,
				    g_session.load(std::memory_order_acquire),
				    GetCurrentProcessId(),
				    GetCurrentThreadId(),
				    thread_role_name(g_thread_role),
				    filename(location.file_name()),
				    location.line(),
				    location.function_name(),
				    event,
				    detail.empty() ? "" : " | ",
				    detail);
				append(line);
			}
			catch (...)
			{
				// Diagnostics must never become a second crash source.
			}
		}
	}

	void set_thread_role(thread_role role) noexcept
	{
		g_thread_role = role;
	}

	thread_role current_thread_role() noexcept
	{
		return g_thread_role;
	}

	const char *thread_role_name(thread_role role) noexcept
	{
		switch (role)
		{
		case thread_role::abi:
			return "abi";
		case thread_role::network:
			return "network";
		case thread_role::kcse_post_update:
			return "kcse-post-update";
		case thread_role::unknown:
		default:
			return "unknown";
		}
	}

	std::uint64_t begin_join(
	    std::string_view server_target,
	    std::source_location location) noexcept
	{
		const auto id = g_session.fetch_add(1, std::memory_order_acq_rel) + 1;
		g_active.store(true, std::memory_order_release);
		try
		{
			write(
			    "join.begin",
			    std::format("target=\"{}\"", server_target),
			    location);
		}
		catch (...)
		{
			write("join.begin", "target formatting failed", location);
		}
		return id;
	}

	void finish_join(
	    std::string_view outcome,
	    std::source_location location) noexcept
	{
		if (!active())
			return;
		const auto enabled = diagnostics_enabled();
		if (enabled)
			write("join.finish", outcome, location);
		g_active.store(false, std::memory_order_release);
		if (enabled)
			flush(true);
	}

	bool active() noexcept
	{
		return g_active.load(std::memory_order_acquire);
	}

	std::uint64_t session_id() noexcept
	{
		return g_session.load(std::memory_order_acquire);
	}

	void set_diagnostics_enabled(bool enabled) noexcept
	{
		const auto was_enabled =
		    g_diagnostics_enabled.exchange(enabled, std::memory_order_acq_rel);
		if (was_enabled && !enabled)
			flush(false);
	}

	bool diagnostics_enabled() noexcept
	{
		return g_diagnostics_enabled.load(std::memory_order_acquire);
	}

	void write(
	    std::string_view event,
	    std::string_view detail,
	    std::source_location location) noexcept
	{
		if (!active() || !diagnostics_enabled())
			return;
		write_line(event, detail, location);
	}

	void write_diagnostic(
	    std::string_view event,
	    std::string_view detail,
	    std::source_location location) noexcept
	{
		if (!diagnostics_enabled())
			return;
		write_line(event, detail, location);
	}

#ifdef _WIN32
	long seh_filter(
	    EXCEPTION_POINTERS *exception,
	    std::string_view event,
	    std::source_location location) noexcept
	{
		try
		{
			const auto *record = exception ? exception->ExceptionRecord : nullptr;
			// Native crashes remain available even when verbose diagnostics are
			// disabled. This path is exceptional and must not depend on a UI or
			// persisted setting being initialized.
			write_line(
			    event,
			    std::format(
			        "SEH code=0x{:08X} address={} flags=0x{:08X}",
			        record ? record->ExceptionCode : 0,
			        record ? record->ExceptionAddress : nullptr,
			        record ? record->ExceptionFlags : 0),
			    location);
			flush(true);
		}
		catch (...)
		{
		}
		return EXCEPTION_EXECUTE_HANDLER;
	}
#endif
}
