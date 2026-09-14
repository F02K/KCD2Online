#include "server/native_game_process.hpp"

#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <Windows.h>

#include <algorithm>
#include <cwchar>
#include <string_view>
#include <vector>
#endif

namespace kcd2o::server
{
	struct native_game_process::implementation
	{
#ifdef _WIN32
		HANDLE process{};
		DWORD process_id{};
		bool hide_window{};
#endif
	};

#ifdef _WIN32
	namespace
	{
		std::wstring widen(std::string_view value)
		{
			if (value.empty())
				return {};
			const auto count = MultiByteToWideChar(
			    CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
			    static_cast<int>(value.size()), nullptr, 0);
			if (count <= 0)
				throw std::runtime_error("native-game launch value is not valid UTF-8");
			std::wstring result(static_cast<std::size_t>(count), L'\0');
			if (MultiByteToWideChar(
			        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
			        static_cast<int>(value.size()), result.data(), count)
			    != count)
				throw std::runtime_error("could not encode native-game launch value");
			return result;
		}

		bool starts_with_environment_name(
		    std::wstring_view entry,
		    std::wstring_view name) noexcept
		{
			return entry.size() > name.size()
			    && entry[name.size()] == L'='
			    && _wcsnicmp(entry.data(), name.data(), name.size()) == 0;
		}

		std::vector<wchar_t> environment_block(
		    const native_game_launch_options &options)
		{
			constexpr std::wstring_view role_name = L"KCD2ONLINE_PROCESS_ROLE";
			constexpr std::wstring_view config_name = L"KCD2ONLINE_SERVER_CONFIG";
			constexpr std::wstring_view endpoint_name = L"KCD2ONLINE_SERVER_ENDPOINT";
			constexpr std::wstring_view level_name = L"KCD2ONLINE_LEVEL_ID";
			const std::wstring_view replaced[]{
			    role_name, config_name, endpoint_name, level_name};

			std::vector<std::wstring> entries;
			const auto *raw = GetEnvironmentStringsW();
			if (!raw)
				throw std::runtime_error("could not read the server environment");
			for (const auto *cursor = raw; *cursor != L'\0';)
			{
				const std::wstring entry(cursor);
				cursor += entry.size() + 1;
				const bool replace = std::ranges::any_of(
				    replaced,
				    [&](std::wstring_view name)
				    { return starts_with_environment_name(entry, name); });
				if (!replace)
					entries.push_back(entry);
			}
			FreeEnvironmentStringsW(const_cast<wchar_t *>(raw));

			entries.emplace_back(L"KCD2ONLINE_PROCESS_ROLE=native_game_host");
			entries.emplace_back(
			    L"KCD2ONLINE_SERVER_CONFIG="
			    + std::filesystem::absolute(options.server_config_path).wstring());
			entries.emplace_back(
			    L"KCD2ONLINE_SERVER_ENDPOINT=" + widen(options.server_endpoint));
			entries.emplace_back(L"KCD2ONLINE_LEVEL_ID=" + widen(options.level_id));
			std::ranges::sort(
			    entries,
			    [](const std::wstring &left, const std::wstring &right)
			    { return _wcsicmp(left.c_str(), right.c_str()) < 0; });

			std::size_t size = 1;
			for (const auto &entry : entries)
				size += entry.size() + 1;
			std::vector<wchar_t> result;
			result.reserve(size);
			for (const auto &entry : entries)
			{
				result.insert(result.end(), entry.begin(), entry.end());
				result.push_back(L'\0');
			}
			result.push_back(L'\0');
			return result;
		}

		BOOL CALLBACK hide_process_window(HWND window, LPARAM parameter)
		{
			DWORD window_process{};
			GetWindowThreadProcessId(window, &window_process);
			if (window_process == static_cast<DWORD>(parameter)
			    && IsWindowVisible(window))
				ShowWindow(window, SW_HIDE);
			return TRUE;
		}

		BOOL CALLBACK close_process_window(HWND window, LPARAM parameter)
		{
			DWORD window_process{};
			GetWindowThreadProcessId(window, &window_process);
			if (window_process == static_cast<DWORD>(parameter))
				PostMessageW(window, WM_CLOSE, 0, 0);
			return TRUE;
		}
	}
#endif

	native_game_process::native_game_process() :
	    m_implementation(std::make_unique<implementation>())
	{
	}

	native_game_process::~native_game_process()
	{
		stop();
	}

	native_game_process::native_game_process(native_game_process &&) noexcept = default;
	native_game_process &native_game_process::operator=(native_game_process &&) noexcept = default;

	void native_game_process::start(const native_game_launch_options &options)
	{
		if (running())
			throw std::logic_error("native-game process is already running");
#ifdef _WIN32
		if (!std::filesystem::is_regular_file(options.installation.executable))
			throw std::runtime_error("native-game executable no longer exists");

		auto command_line = L'"' + options.installation.executable.wstring() + L'"';
		auto environment = environment_block(options);
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESHOWWINDOW;
		startup.wShowWindow = options.hide_window ? SW_HIDE : SW_SHOWDEFAULT;
		PROCESS_INFORMATION process{};
		const auto created = CreateProcessW(
		    options.installation.executable.c_str(), command_line.data(), nullptr,
		    nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP,
		    environment.data(), options.installation.executable.parent_path().c_str(),
		    &startup, &process);
		if (!created)
			throw std::runtime_error(
			    "could not launch native-game process (Windows error "
			    + std::to_string(GetLastError()) + ")");
		CloseHandle(process.hThread);
		m_implementation->process = process.hProcess;
		m_implementation->process_id = process.dwProcessId;
		m_implementation->hide_window = options.hide_window;
		maintain_hidden_window();
#else
		(void)options;
		throw std::runtime_error("native-game simulation is supported only on Windows");
#endif
	}

	void native_game_process::stop() noexcept
	{
#ifdef _WIN32
		if (!m_implementation || !m_implementation->process)
			return;
		if (running())
		{
			EnumWindows(
			    close_process_window,
			    static_cast<LPARAM>(m_implementation->process_id));
			if (WaitForSingleObject(m_implementation->process, 5'000) == WAIT_TIMEOUT)
			{
				TerminateProcess(m_implementation->process, 0);
				WaitForSingleObject(m_implementation->process, 2'000);
			}
		}
		CloseHandle(m_implementation->process);
		m_implementation->process = nullptr;
		m_implementation->process_id = 0;
#endif
	}

	void native_game_process::maintain_hidden_window() const noexcept
	{
#ifdef _WIN32
		if (m_implementation && m_implementation->hide_window && running())
			EnumWindows(
			    hide_process_window,
			    static_cast<LPARAM>(m_implementation->process_id));
#endif
	}

	bool native_game_process::running() const noexcept
	{
#ifdef _WIN32
		if (!m_implementation || !m_implementation->process)
			return false;
		DWORD code{};
		return GetExitCodeProcess(m_implementation->process, &code)
		    && code == STILL_ACTIVE;
#else
		return false;
#endif
	}

	std::optional<std::uint32_t> native_game_process::exit_code() const noexcept
	{
#ifdef _WIN32
		if (!m_implementation || !m_implementation->process)
			return std::nullopt;
		DWORD code{};
		if (!GetExitCodeProcess(m_implementation->process, &code)
		    || code == STILL_ACTIVE)
			return std::nullopt;
		return code;
#else
		return std::nullopt;
#endif
	}

	std::uint32_t native_game_process::process_id() const noexcept
	{
#ifdef _WIN32
		return m_implementation ? m_implementation->process_id : 0;
#else
		return 0;
#endif
	}
}
