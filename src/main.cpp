#include "asi_loader/asi_loader.hpp"
#include "config/config.hpp"
#include "dll_proxy/dll_proxy.hpp"
#include "gui/renderer.hpp"
#include "hooks/hooking.hpp"
#include "input/hotkey.hpp"
#include "kcd2_init.hpp"
#include "kcse/client_proxy.hpp"
#include "logger/exception_handler.hpp"
#include "memory/byte_patch_manager.hpp"
#include "memory/module.hpp"
#include "paths/paths.hpp"
#include "threads/thread_pool.hpp"
#include "version.hpp"

#include <mimalloc-new-delete.h>

namespace
{
	constexpr DWORD SUPPORTED_WHGAME_TIMESTAMP = 0x6A350E20;
	constexpr size_t SUPPORTED_WHGAME_IMAGE_SIZE = 0x5B2D000;
	constexpr auto WHGAME_WAIT_TIMEOUT = std::chrono::seconds(30);

	void flush_logs()
	{
		if (big::g_log)
		{
			al::Logger::FlushQueue();
		}
	}

	bool initialize_kcd2o(HMODULE module)
	{
		using namespace big;
		const auto started_at = std::chrono::steady_clock::now();

		if (!rom::is_rom_enabled())
		{
			return false;
		}

		rom::init("KCD2Online", "WHGame.dll", "rom");
		setlocale(LC_ALL, ".utf8");

		const std::filesystem::path root_folder = paths::get_project_root_folder();
		g_file_manager.init(root_folder);
		paths::init_dump_file_path();
		config::init_general();

		// KCD2Online is currently a development fork. Keep the diagnostic console
		// available in both Debug and Release, even if an older config disabled it.
		config::general()
		    .bind("Logging", "Console Enabled", true, "KCD2Online always displays its diagnostic console.")
		    ->set_value(true);

		// Intentionally leaked: the proxy DLL remains loaded for the process lifetime.
		new logger(rom::g_project_name, g_file_manager.get_project_file("./LogOutput.log"));
		// Keep ROM's dump-producing top-level filter, but do not detour
		// SetUnhandledExceptionFilter. That anti-removal detour is not required
		// for KCD2Online and failed during startup on the supported game build.
		new exception_handler(false, big_exception_handler);

		LOG(INFO) << "KCD2Online bootstrap thread started outside DllMain.";
		LOGF(INFO, "Build (GIT SHA1): {}", version::GIT_SHA1);
#ifdef FINAL
		LOG(INFO) << "Build profile: Release (FINAL)";
#else
		LOG(INFO) << "Build profile: Debug";
#endif

		memory::module whgame("WHGame.dll");
		if (!whgame.wait_for_module(WHGAME_WAIT_TIMEOUT))
		{
			LOG(ERROR) << "WHGame.dll did not load within 30 seconds. KCD2Online hooks will remain disabled; the game may continue without the mod.";
			flush_logs();
			return false;
		}

		LOGF(
		    INFO,
		    "Detected WHGame.dll: TimeDateStamp=0x{:08X}, SizeOfImage=0x{:X}",
		    whgame.timestamp(),
		    whgame.size());

		if (whgame.timestamp() != SUPPORTED_WHGAME_TIMESTAMP || whgame.size() != SUPPORTED_WHGAME_IMAGE_SIZE)
		{
			LOGF(
			    ERROR,
			    "Unsupported WHGame.dll build. Expected TimeDateStamp=0x{:08X}, SizeOfImage=0x{:X}. KCD2Online hooks will remain disabled; the game will continue without the mod.",
			    SUPPORTED_WHGAME_TIMESTAMP,
			    SUPPORTED_WHGAME_IMAGE_SIZE);
			flush_logs();
			return false;
		}

		LOG(INFO) << "Supported Steam build 23914554 / WHGame build 1308617_856 detected.";

		std::srand(static_cast<unsigned int>(std::chrono::system_clock::now().time_since_epoch().count()));

		// These process-lifetime services are intentionally leaked. They are only
		// created after DllMain returned, so their worker threads cannot deadlock
		// against the Windows loader lock.
		new thread_pool();
		new byte_patch_manager();
		LOG(INFO) << "Runtime services initialized.";

		const auto init_result = kcd2_init();
		if (!init_result.success)
		{
			LOGF(
			    ERROR,
			    "KCD2Online address validation failed ({}/{} signatures, {}/{} derived targets). No hooks will be enabled.",
			    init_result.signatures_resolved,
			    init_result.signatures_requested,
			    init_result.derived_resolved,
			    init_result.derived_requested);
			for (const auto &error : init_result.errors)
			{
				LOG(ERROR) << "  - " << error;
			}
			LOG(ERROR) << "The game will continue without KCD2Online hooks.";
			flush_logs();
			return false;
		}

		LOGF(
		    INFO,
		    "Signature validation completed: {}/{} signatures resolved.",
		    init_result.signatures_resolved,
		    init_result.signatures_requested);
		LOGF(
		    INFO,
		    "Derived target validation completed: {}/{} valid.",
		    init_result.derived_resolved,
		    init_result.derived_requested);

		new hooking();
		LOG(INFO) << "Hook objects initialized.";

		new renderer();
		LOG(INFO) << "Renderer initialized.";

		hotkey::init_hotkeys();
		g_hooking->enable();
		LOG(INFO) << "Hooks enabled.";

		const auto kcse_status =
		    kcd2o::kcse::ui_client().runtime_capability();
		if (kcd2o::kcse::ui_client().available())
		{
			LOG(INFO) << "KCSE-owned multiplayer client detected.";
			if (!kcse_status.available)
				LOG(INFO) << kcse_status.diagnostic;
		}
		else
		{
			LOG(INFO)
			    << "KCD2OnlineKCSEClient.dll is not loaded; the UI remains "
			       "available but in-game multiplayer is disabled.";
		}

		asi_loader::init(module);
		g_running = true;

		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		    std::chrono::steady_clock::now() - started_at);
		LOGF(
		    INFO,
		    "KCD2Online initialization completed - {}/{} signatures resolved - hooks enabled ({} ms).",
		    init_result.signatures_resolved,
		    init_result.signatures_requested,
		    elapsed.count());
		flush_logs();
		return true;
	}

	DWORD WINAPI bootstrap_thread(PVOID parameter) noexcept
	{
		const auto module = static_cast<HMODULE>(parameter);
		try
		{
			initialize_kcd2o(module);
		}
		catch (const std::exception &exception)
		{
			if (big::g_log)
			{
				LOG(ERROR) << "Unhandled exception during KCD2Online initialization: " << exception.what();
				LOG(ERROR) << "KCD2Online hooks remain disabled; the game may continue without the mod.";
				flush_logs();
			}
			else
			{
				OutputDebugStringA("KCD2Online initialization failed before the logger was available.\n");
			}
		}
		catch (...)
		{
			if (big::g_log)
			{
				LOG(ERROR) << "Unknown exception during KCD2Online initialization.";
				LOG(ERROR) << "KCD2Online hooks remain disabled; the game may continue without the mod.";
				flush_logs();
			}
			else
			{
				OutputDebugStringA("KCD2Online initialization failed with an unknown exception.\n");
			}
		}
		return 0;
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, PVOID)
{
	if (reason != DLL_PROCESS_ATTACH)
	{
		return TRUE;
	}

	if (!big::dll_proxy::init())
	{
		return FALSE;
	}

	big::g_hmodule = module;
	DisableThreadLibraryCalls(module);

	DWORD thread_id{};
	const auto thread = CreateThread(nullptr, 0, bootstrap_thread, module, 0, &thread_id);
	if (!thread)
	{
		// The D3D12 proxy remains usable even when mod initialization cannot start.
		OutputDebugStringA("KCD2Online failed to create its bootstrap thread; continuing as a D3D12 proxy only.\n");
		return TRUE;
	}

	big::g_main_thread_id = thread_id;
	CloseHandle(thread);
	return TRUE;
}
