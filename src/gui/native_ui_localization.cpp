#include "gui/native_ui_localization.hpp"

#include <Windows.h>

#include <Offsets/vtables/IConsole.h>
#include <Offsets/vtables/ICVar.h>
#include <crysystem/SSystemGlobalEnvironment.h>

#include <array>
#include <filesystem>
#include <mutex>
#include <utility>

namespace big::ingame_ui
{
	namespace
	{
		struct localization_state
		{
			std::mutex mutex;
			localization_catalog catalog;
			std::string requested_language;
			std::string last_error;
		};

		localization_state &state()
		{
			static localization_state value;
			return value;
		}

		std::filesystem::path language_directory()
		{
			std::array<wchar_t, 32768> path{};
			const auto length = GetModuleFileNameW(
			    nullptr,
			    path.data(),
			    static_cast<DWORD>(path.size()));
			if (length == 0
			    || length >= static_cast<DWORD>(path.size()))
				return {};
			const std::filesystem::path executable(
			    std::wstring_view(path.data(), length));
			return executable.parent_path().parent_path().parent_path()
			    / "mods" / "KCD2Online" / "Lang";
		}

		const char *guarded_native_game_language() noexcept
		{
#ifdef _WIN32
			__try
			{
#endif
				auto *environment = SSystemGlobalEnvironment::GetInstance();
				auto *console = environment ? environment->pConsole : nullptr;
				if (!console)
					return "en";
				auto *language = console->GetCVar("g_language");
				const auto *value = language ? language->GetString() : nullptr;
				return value && value[0] != '\0' ? value : "en";
#ifdef _WIN32
			}
			__except(EXCEPTION_EXECUTE_HANDLER)
			{
				return "en";
			}
#endif
		}

		std::string native_game_language()
		{
			return guarded_native_game_language();
		}

		void refresh(localization_state &value)
		{
			const auto requested = normalize_language(native_game_language());
			if (requested == value.requested_language)
				return;
			std::string error;
			if (value.catalog.load(language_directory(), requested, error))
			{
				value.requested_language = requested;
				value.last_error.clear();
				return;
			}
			value.requested_language = requested;
			if (error != value.last_error)
			{
				value.last_error = std::move(error);
				const auto diagnostic =
				    "KCD2Online localization: " + value.last_error + "\n";
				OutputDebugStringA(diagnostic.c_str());
			}
		}
	}

	std::string localized(std::string_view key)
	{
		auto &value = state();
		std::scoped_lock lock(value.mutex);
		refresh(value);
		return value.catalog.text(key);
	}

	std::string localized(
	    std::string_view key,
	    std::initializer_list<format_argument> arguments)
	{
		auto &value = state();
		std::scoped_lock lock(value.mutex);
		refresh(value);
		return value.catalog.format(key, arguments);
	}

	std::string active_game_language()
	{
		auto &value = state();
		std::scoped_lock lock(value.mutex);
		refresh(value);
		return value.catalog.language();
	}
}
