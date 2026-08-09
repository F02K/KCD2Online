#include "gui/native_multiplayer_menu.hpp"

#include "account/account_service.hpp"
#include "account/server_directory.hpp"
#include "gui/native_ingame_menu_api.hpp"
#include "gui/native_ui_localization.hpp"

#include "kcse/client_proxy.hpp"
#include "multiplayer/ui_settings.hpp"

#include <Offsets/vtables/IUIElement.h>
#include <Offsets/vtables/IUIElementEventListener.h>
#include <guimodule/SUIEventDesc.h>
#include <guimodule/SUITypes.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <utility>

namespace big::native_multiplayer_menu
{
	namespace
	{
		constexpr std::uint8_t root_main_page = 1;
		constexpr std::uint8_t root_ingame_page = 2;
		constexpr std::uint8_t custom_multiplayer_page = 0xFE;
		constexpr std::uint8_t custom_death_page = 0xFD;
		constexpr std::uint8_t custom_account_page = 0xFC;

		constexpr std::string_view multiplayer_button =
		    "KCD2Online.Multiplayer";
		constexpr std::string_view name_button = "KCD2Online.Name";
		constexpr std::string_view password_button = "KCD2Online.Password";
		constexpr std::string_view connect_button = "KCD2Online.Connect";
		constexpr std::string_view disconnect_button = "KCD2Online.Disconnect";
		constexpr std::string_view cancel_pending_button =
		    "KCD2Online.CancelPending";
		constexpr std::string_view status_button = "KCD2Online.Status";
		constexpr std::string_view back_button = "KCD2Online.Back";
		constexpr std::string_view respawn_button = "KCD2Online.Respawn";
		constexpr std::string_view account_button = "KCD2Online.Account";
		constexpr std::string_view account_accept_button = "KCD2Online.Account.Accept";
		constexpr std::string_view account_decline_button = "KCD2Online.Account.Decline";
		constexpr std::string_view account_retry_button = "KCD2Online.Account.Retry";
		constexpr std::string_view account_continue_button = "KCD2Online.Account.Continue";
		constexpr std::string_view account_details_button = "KCD2Online.Account.Details";
		constexpr std::string_view account_copy_button = "KCD2Online.Account.Copy";
		constexpr std::string_view account_disable_button = "KCD2Online.Account.Disable";
		constexpr std::string_view refresh_button = "KCD2Online.Servers.Refresh";
		constexpr std::string_view favorite_button = "KCD2Online.Server.Favorite";
		constexpr std::string_view browser_back_button = "KCD2Online.Server.Back";
		constexpr std::string_view server_button_prefix = "KCD2Online.Server.Select.";

		enum class edit_field
		{
			none,
			name,
			password,
		};

		struct state;
		state &menu_state();

		std::string narrow_utf8(std::wstring_view value)
		{
			if (value.empty())
				return {};
			const auto size = WideCharToMultiByte(
			    CP_UTF8,
			    0,
			    value.data(),
			    static_cast<int>(value.size()),
			    nullptr,
			    0,
			    nullptr,
			    nullptr);
			if (size <= 0)
				return {};
			std::string result(static_cast<std::size_t>(size), '\0');
			WideCharToMultiByte(
			    CP_UTF8,
			    0,
			    value.data(),
			    static_cast<int>(value.size()),
			    result.data(),
			    size,
			    nullptr,
			    nullptr);
			return result;
		}

		std::string clipboard_text()
		{
			if (!OpenClipboard(nullptr))
				return {};
			struct clipboard_guard
			{
				~clipboard_guard()
				{
					CloseClipboard();
				}
			} guard;
			const auto handle = GetClipboardData(CF_UNICODETEXT);
			if (!handle)
				return {};
			const auto *text = static_cast<const wchar_t *>(GlobalLock(handle));
			if (!text)
				return {};
			const std::wstring value(text);
			GlobalUnlock(handle);
			return narrow_utf8(value);
		}

		bool set_clipboard_text(std::string_view text)
		{
			const auto length = MultiByteToWideChar(
			    CP_UTF8,
			    MB_ERR_INVALID_CHARS,
			    text.data(),
			    static_cast<int>(text.size()),
			    nullptr,
			    0);
			if (length <= 0 || !OpenClipboard(nullptr))
				return false;
			struct clipboard_guard
			{
				~clipboard_guard()
				{
					CloseClipboard();
				}
			} guard;
			if (!EmptyClipboard())
				return false;
			const auto bytes = static_cast<SIZE_T>(length + 1) * sizeof(wchar_t);
			const auto handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
			if (!handle)
				return false;
			auto *target = static_cast<wchar_t *>(GlobalLock(handle));
			if (!target)
			{
				GlobalFree(handle);
				return false;
			}
			MultiByteToWideChar(
			    CP_UTF8,
			    MB_ERR_INVALID_CHARS,
			    text.data(),
			    static_cast<int>(text.size()),
			    target,
			    length);
			target[length] = L'\0';
			GlobalUnlock(handle);
			if (!SetClipboardData(CF_UNICODETEXT, handle))
			{
				GlobalFree(handle);
				return false;
			}
			return true;
		}

		void erase_last_utf8_character(std::string &text)
		{
			if (text.empty())
				return;
			auto offset = text.size() - 1;
			while (offset > 0
			    && (static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80)
				--offset;
			text.erase(offset);
		}

		std::string_view utf8_prefix(
		    std::string_view text,
		    std::size_t capacity)
		{
			std::size_t offset{};
			while (offset < text.size())
			{
				const auto lead = static_cast<unsigned char>(text[offset]);
				const std::size_t length = lead < 0x80 ? 1
				    : (lead & 0xE0) == 0xC0       ? 2
				    : (lead & 0xF0) == 0xE0       ? 3
				    : (lead & 0xF8) == 0xF0       ? 4
				                                  : 1;
				if (offset + length > text.size()
				    || offset + length > capacity)
					break;
				offset += length;
			}
			return text.substr(0, offset);
		}

		class listener final : public Offsets::IUIElementEventListener
		{
		public:
			void OnUIEvent(
			    Offsets::IUIElement *,
			    const SUIEventDesc &event,
			    const SUIArguments &arguments,
			    void *) override;
			void OnUIEventEx(
			    Offsets::IUIElement *,
			    const char *,
			    const SUIArguments &,
			    void *) override
			{
			}
			void OnInit(
			    Offsets::IUIElement *,
			    Offsets::IFlashPlayer *) override
			{
			}
			void OnUnload(Offsets::IUIElement *sender) override;
			void OnSetVisible(
			    Offsets::IUIElement *sender,
			    bool visible) override;
			void OnInstanceCreated(
			    Offsets::IUIElement *,
			    Offsets::IUIElement *) override
			{
			}
			void OnInstanceDestroyed(
			    Offsets::IUIElement *,
			    Offsets::IUIElement *) override
			{
			}
			void Dtor(char) override
			{
			}
		};

		struct state
		{
			std::mutex mutex;
			listener event_listener;
			void *menu{};
			Offsets::IUIElement *element{};
			bool listener_attached{};
			bool page_open{};
			bool account_page_open{};
			bool death_page_open{};
			bool account_details{};
			bool rebuild_requested{};
			bool pending_join{};
			edit_field editing{edit_field::none};
			std::string edit_original;
			std::string password;
			std::string selected_server_id;
			std::string queued_button;
			std::string local_feedback_key;
			std::string last_status_text;
			std::string presented_error;
			std::uint64_t last_account_revision{};
			std::uint64_t last_directory_revision{};
			bool directory_started{};
			std::chrono::steady_clock::time_point next_status_poll;
		};

		state &menu_state()
		{
			static state value;
			return value;
		}

		std::string client_state_text(const kcd2o::client_status &status)
		{
			using kcd2o::client_state;
			if (!status.error.empty())
				return "Connection failed: " + status.error;
			switch (status.state)
			{
			case client_state::disconnected:
				return kcd2o::kcse::ui_client().can_start_join()
				    ? ingame_ui::localized("menu.status.ready")
				    : ingame_ui::localized("menu.status.kcse_preparing");
			case client_state::runtime_preflight:
				return ingame_ui::localized("menu.status.preflight");
			case client_state::connecting:
				return ingame_ui::localized("menu.status.connecting");
			case client_state::preflight:
				return ingame_ui::localized("menu.status.negotiating");
			case client_state::authenticating:
				return ingame_ui::localized("menu.status.authenticating");
			case client_state::waiting_for_bootstrap:
				return ingame_ui::localized("menu.status.waiting_server");
			case client_state::loading_sandbox:
				return ingame_ui::localized("menu.status.loading_world");
			case client_state::applying_profile:
				return ingame_ui::localized("menu.status.applying_profile");
			case client_state::connected:
				return status.server_name.empty()
				    ? ingame_ui::localized("menu.status.connected")
				    : ingame_ui::localized(
				        "menu.status.connected_server",
				        {{"server", status.server_name}});
			case client_state::reconnecting:
				return ingame_ui::localized("menu.status.reconnecting");
			case client_state::closing:
				return ingame_ui::localized("menu.status.closing");
			}
			return ingame_ui::localized("menu.status.multiplayer");
		}

		std::string &edited_value(state &value)
		{
			auto &settings = kcd2o::ui_settings();
			switch (value.editing)
			{
			case edit_field::name: return settings.display_name;
			case edit_field::password: return value.password;
			case edit_field::none: break;
			}
			return value.password;
		}

		void persist_edit(edit_field field)
		{
			auto &settings = kcd2o::ui_settings();
			if (field == edit_field::name)
				settings.persist_display_name();
		}

		std::string short_account_id(std::string_view account_id)
		{
			if (account_id.size() <= 18)
				return std::string(account_id);
			return std::string(account_id.substr(0, 8)) + "..."
			    + std::string(account_id.substr(account_id.size() - 4));
		}

		std::string paragraphs(
		    std::initializer_list<std::string> values)
		{
			std::string result;
			for (const auto &value : values)
			{
				if (value.empty())
					continue;
				if (!result.empty())
					result += "\n\n";
				result += value;
			}
			return result;
		}

		void show_account_page()
		{
			auto &value = menu_state();
			void *menu{};
			bool details{};
			{
				std::scoped_lock lock(value.mutex);
				menu = value.menu;
				details = value.account_details;
				value.page_open = false;
				value.death_page_open = false;
				value.rebuild_requested = false;
			}
			ingame_ui::native_menu_api api(menu);
			const auto account = kcd2o::account::service().status();
			{
				std::scoped_lock lock(value.mutex);
				value.account_page_open = api.available();
				value.last_account_revision = account.revision;
			}
			if (!api.available())
				return;

			ingame_ui::page page;
			page.id = custom_account_page;
			page.height = 420;
			page.visible_rows = 9;
			page.title = ingame_ui::localized("account.title");
			const auto scroll_hint =
			    ingame_ui::localized("account.info.scroll_hint");

			if (details)
			{
				page.information = ingame_ui::information_panel{
				    ingame_ui::localized("account.info.details_title"),
				    paragraphs({
				        ingame_ui::localized("account.info.identity"),
				        ingame_ui::localized("account.info.no_profile"),
				        ingame_ui::localized("account.info.device"),
				        ingame_ui::localized("account.info.required")}),
				    scroll_hint};
				page.add_button(std::string(account_details_button), ingame_ui::localized("account.action.details_back"));
				page.add_button(std::string(back_button), ingame_ui::localized("menu.action.back"), false, 1);
				page.selected_button = account_details_button;
				(void)api.show(page);
				return;
			}

			using kcd2o::account::account_state;
			switch (account.state)
			{
			case account_state::undecided:
				page.information = ingame_ui::information_panel{
				    ingame_ui::localized("account.info.setup_title"),
				    paragraphs({
				        ingame_ui::localized("account.info.intro"),
				        ingame_ui::localized("account.info.no_profile"),
				        ingame_ui::localized("account.info.consent")}),
				    scroll_hint};
				page.add_button(std::string(account_accept_button), ingame_ui::localized("account.action.accept"));
				page.add_button(std::string(account_decline_button), ingame_ui::localized("account.action.decline"));
				page.selected_button = account_accept_button;
				break;
			case account_state::declined:
				page.information = ingame_ui::information_panel{
				    ingame_ui::localized("account.info.disabled_title"),
				    paragraphs({
				        ingame_ui::localized("account.status.disabled"),
				        ingame_ui::localized("account.info.consent")}),
				    scroll_hint};
				page.add_button(std::string(account_accept_button), ingame_ui::localized("account.action.enable"));
				page.selected_button = account_accept_button;
				break;
			case account_state::registering:
				page.information = ingame_ui::information_panel{
				    ingame_ui::localized("account.info.registering_title"),
				    ingame_ui::localized("account.status.registering"),
				    scroll_hint};
				break;
			case account_state::ready:
				page.information = ingame_ui::information_panel{
				    ingame_ui::localized("account.info.ready_title"),
				    paragraphs({
				        ingame_ui::localized("account.status.ready"),
				        ingame_ui::localized(
				            "account.id", {{"id", account.account_id}})}),
				    scroll_hint};
				page.add_button(
				    std::string(account_copy_button),
				    ingame_ui::localized("account.action.copy"),
				    false,
				    0,
				    ingame_ui::localized("account.action.copy_tooltip"));
				page.add_button(std::string(account_continue_button), ingame_ui::localized("account.action.continue"));
				page.add_button(std::string(account_disable_button), ingame_ui::localized("account.action.disable"));
				page.selected_button = account_continue_button;
				break;
			case account_state::error:
				page.information = ingame_ui::information_panel{
				    ingame_ui::localized("account.info.error_title"),
				    ingame_ui::localized(
				        account.error_key.empty()
				            ? "account.error.service"
				            : account.error_key),
				    scroll_hint};
				page.add_button(std::string(account_retry_button), ingame_ui::localized("account.action.retry"));
				page.add_button(std::string(account_decline_button), ingame_ui::localized("account.action.decline"));
				page.selected_button = account_retry_button;
				break;
			}

			if (account.state != account_state::registering)
				page.add_button(std::string(account_details_button), ingame_ui::localized("account.action.details"));
			page.add_button(std::string(back_button), ingame_ui::localized("menu.action.back"), false, 1);
			if (page.selected_button.empty())
				page.selected_button = back_button;
			(void)api.show(page);
		}

		void show_multiplayer_page()
		{
			if (!kcd2o::account::service().status().multiplayer_enabled())
			{
				show_account_page();
				return;
			}
			auto &value = menu_state();
			void *menu{};
			edit_field editing{};
			bool pending_join{};
			bool start_directory{};
			std::string password;
			std::string selected_id;
			{
				std::scoped_lock lock(value.mutex);
				menu = value.menu;
				editing = value.editing;
				pending_join = value.pending_join;
				password = value.password;
				selected_id = value.selected_server_id;
				start_directory = !value.directory_started;
				value.directory_started = true;
				value.account_page_open = false;
				value.rebuild_requested = false;
			}
			if (start_directory)
				kcd2o::account::directory().refresh(kcd2o::ui_settings().account_service_url);
			ingame_ui::native_menu_api api(menu);
			{
				std::scoped_lock lock(value.mutex);
				value.page_open = api.available();
			}
			if (!api.available())
				return;

			const auto status = kcd2o::kcse::ui_client().status();
			const auto directory = kcd2o::account::directory().snapshot();
			const auto selected = std::ranges::find_if(
			    directory.servers,
			    [&](const kcd2o::account::public_server &server)
			    { return server.id == selected_id; });
			ingame_ui::page page;
			page.id = custom_multiplayer_page;
			page.title = status.error.empty()
			    ? ingame_ui::localized("menu.multiplayer.title")
			    : "Multiplayer connection failed";

			auto &settings = kcd2o::ui_settings();
			const bool settings_locked = pending_join
			    || status.state != kcd2o::client_state::disconnected;
			const auto edit_prefix =
			    ingame_ui::localized("menu.field.edit_prefix") + " ";
			const auto account = kcd2o::account::service().status();
			page.add_button(
			    std::string(name_button),
			    (editing == edit_field::name ? edit_prefix : std::string{})
			        + ingame_ui::localized(
			            "menu.field.name", {{"value", settings.display_name}}),
			    settings_locked,
			    0,
			    ingame_ui::localized("menu.field.name.tooltip"));
			if (!status.error.empty())
			{
				page.information = ingame_ui::information_panel{
				    "Multiplayer connection failed",
				    status.error,
				    ingame_ui::localized("account.info.scroll_hint")};
				page.add_button(std::string(account_button), ingame_ui::localized("account.summary", {{"id", short_account_id(account.account_id)}}));
				page.add_button(std::string(refresh_button), ingame_ui::localized("browser.action.refresh"), directory.loading);
				for (const auto &server : directory.servers)
				{
					const auto label = std::string(kcd2o::account::directory().favorite(server.id) ? "* " : "")
					    + server.name + "  " + std::to_string(server.player_count) + "/" + std::to_string(server.max_players)
					    + (server.password_protected ? " [LOCK]" : "");
					page.add_button(std::string(server_button_prefix) + server.id, label, server.player_count >= server.max_players);
				}
			}
			else if (selected != directory.servers.end())
			{
				const auto &server = *selected;
				page.information = ingame_ui::information_panel{
				    server.name,
				    ingame_ui::localized("browser.details", {
				        {"players", std::to_string(server.player_count)},
				        {"max", std::to_string(server.max_players)},
				        {"level", server.level_id},
				        {"version", server.version},
				        {"id", server.id},
				        {"password", server.password_protected
				            ? ingame_ui::localized("browser.password.yes")
				            : ingame_ui::localized("browser.password.no")}}),
				    ingame_ui::localized("account.info.scroll_hint")};
				if (server.password_protected)
				{
					const auto stored = kcd2o::account::directory().password(server.id);
					if (password.empty() && stored)
						password = *stored;
					const auto password_text = password.empty()
					    ? ingame_ui::localized("menu.field.password.empty")
					    : std::string(password.size(), '*');
					page.add_button(std::string(password_button),
					    (editing == edit_field::password ? edit_prefix : std::string{})
					        + ingame_ui::localized("menu.field.password", {{"value", password_text}}),
					    settings_locked);
				}
				page.add_button(std::string(connect_button),
				    ingame_ui::localized("menu.action.connect"),
				    settings_locked || settings.display_name.empty()
				        || (server.password_protected && password.empty()));
				page.add_button(std::string(favorite_button),
				    ingame_ui::localized(kcd2o::account::directory().favorite(server.id)
				        ? "browser.action.unfavorite" : "browser.action.favorite"));
				page.add_button(std::string(browser_back_button), ingame_ui::localized("browser.action.back"), false, 1);
			}
			else if (status.state != kcd2o::client_state::disconnected)
			{
				page.information = ingame_ui::information_panel{ingame_ui::localized("browser.title"), client_state_text(status), {}};
				page.add_button(std::string(disconnect_button), ingame_ui::localized("menu.action.disconnect"));
			}
			else
			{
				page.information = ingame_ui::information_panel{
				    ingame_ui::localized("browser.title"),
				    directory.loading ? ingame_ui::localized("browser.loading")
				        : !directory.error.empty() ? directory.error
				        : directory.servers.empty() ? ingame_ui::localized("browser.empty")
				        : ingame_ui::localized("browser.info", {{"count", std::to_string(directory.servers.size())}}),
				    ingame_ui::localized("account.info.scroll_hint")};
				page.add_button(std::string(account_button), ingame_ui::localized("account.summary", {{"id", short_account_id(account.account_id)}}));
				page.add_button(std::string(refresh_button), ingame_ui::localized("browser.action.refresh"), directory.loading);
				for (const auto &server : directory.servers)
				{
					const auto label = std::string(kcd2o::account::directory().favorite(server.id) ? "* " : "")
					    + server.name + "  " + std::to_string(server.player_count) + "/" + std::to_string(server.max_players)
					    + (server.password_protected ? " [LOCK]" : "");
					page.add_button(std::string(server_button_prefix) + server.id, label, server.player_count >= server.max_players);
				}
			}
			std::string local_feedback_key;
			{
				std::scoped_lock lock(value.mutex);
				local_feedback_key = value.local_feedback_key;
			}
			const auto status_text = editing != edit_field::none
			    ? ingame_ui::localized("menu.status.editing")
			    : !local_feedback_key.empty()
			    ? ingame_ui::localized(local_feedback_key)
			    : client_state_text(status);
			if (pending_join)
				page.add_button(std::string(cancel_pending_button), ingame_ui::localized("menu.action.cancel_pending"));
			page.add_button(
			    std::string(back_button),
			    ingame_ui::localized("menu.action.back"),
			    false,
			    1);

			switch (editing)
			{
			case edit_field::name:
				page.selected_button = name_button;
				break;
			case edit_field::password:
				page.selected_button = password_button;
				break;
			case edit_field::none:
				page.selected_button = pending_join ? cancel_pending_button
				    : status.state != kcd2o::client_state::disconnected ? disconnect_button
				    : !status.error.empty() ? refresh_button
				    : selected != directory.servers.end() ? connect_button
				    : refresh_button;
				break;
			}
			(void)api.show(page);
			{
				std::scoped_lock lock(value.mutex);
				value.last_status_text = status_text;
				value.last_directory_revision = directory.revision;
				value.next_status_poll =
				    std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
			}
		}

		void show_death_page(bool finalize)
		{
			auto &value = menu_state();
			void *menu{};
			{
				std::scoped_lock lock(value.mutex);
				menu = value.menu;
				value.page_open = false;
				value.account_page_open = false;
				value.rebuild_requested = false;
			}
			ingame_ui::native_menu_api api(menu);
			{
				std::scoped_lock lock(value.mutex);
				value.death_page_open = api.available();
			}
			if (!api.available())
				return;

			const auto status = kcd2o::kcse::ui_client().status();
			ingame_ui::page page;
			page.id = custom_death_page;
			page.visible_rows = 3;
			page.title = ingame_ui::localized("menu.death.title");
			page.finalize = finalize;
			page.selected_button = respawn_button;
			page.add_button(
			    std::string(respawn_button),
			    status.respawn_pending
			        ? ingame_ui::localized("menu.death.respawn_pending")
			        : ingame_ui::localized("menu.death.respawn"),
			    status.respawn_pending,
			    0,
			    ingame_ui::localized("menu.death.respawn_tooltip"));
			page.add_button(
			    std::string(status_button),
			    ingame_ui::localized("menu.death.no_save"),
			    true);
			(void)api.show(page);
		}

		void close_menu()
		{
			auto &value = menu_state();
			void *menu{};
			{
				std::scoped_lock lock(value.mutex);
				menu = value.menu;
				value.page_open = false;
				value.account_page_open = false;
				value.death_page_open = false;
				value.rebuild_requested = false;
			}
			if (!menu)
				return;
			ingame_ui::native_menu_api(menu).close();
		}

		void open_root_menu()
		{
			auto &value = menu_state();
			void *menu{};
			{
				std::scoped_lock lock(value.mutex);
				menu = value.menu;
			}
			if (!menu)
				return;
			if (!ingame_ui::native_menu_api(menu).open_root())
				return;
			std::scoped_lock lock(value.mutex);
			value.page_open = false;
			value.account_page_open = false;
			value.account_details = false;
			value.editing = edit_field::none;
			value.rebuild_requested = false;
		}

		void begin_edit(edit_field field)
		{
			auto &value = menu_state();
			std::scoped_lock lock(value.mutex);
			value.editing = field;
			value.edit_original = edited_value(value);
			value.local_feedback_key.clear();
			value.rebuild_requested = true;
		}

		void connect_or_defer()
		{
			if (!kcd2o::account::service().status().multiplayer_enabled())
			{
				show_account_page();
				return;
			}
			auto &value = menu_state();
			kcd2o::client_options options;
			std::string selected_id;
			{
				std::scoped_lock lock(value.mutex);
				auto &settings = kcd2o::ui_settings();
				settings.persist_display_name();
				options.display_name = settings.display_name;
				options.password = value.password;
				options.account_service_url = settings.account_service_url;
				selected_id = value.selected_server_id;
			}
			const auto directory = kcd2o::account::directory().snapshot();
			const auto selected = std::ranges::find_if(directory.servers,
			    [&](const kcd2o::account::public_server &server) { return server.id == selected_id; });
			if (selected == directory.servers.end() || options.display_name.empty())
			{
				std::scoped_lock lock(value.mutex);
				value.local_feedback_key = "menu.feedback.required";
				value.rebuild_requested = true;
				return;
			}
			options.address = selected->address;
			options.server_id = selected->id;
			if (selected->password_protected)
			{
				if (options.password.empty())
				{
					begin_edit(edit_field::password);
					return;
				}
				kcd2o::account::directory().set_password(selected->id, options.password);
			}

			if (kcd2o::kcse::ui_client().can_start_join())
			{
				const auto started = kcd2o::kcse::ui_client().connect(options);
				std::scoped_lock lock(value.mutex);
				if (started)
				{
					value.selected_server_id.clear();
					value.password.clear();
					value.local_feedback_key.clear();
				}
				else
				{
					value.local_feedback_key = "menu.feedback.connect_failed";
				}
				value.rebuild_requested = true;
				return;
			}

			{
				std::scoped_lock lock(value.mutex);
				value.pending_join = true;
				value.local_feedback_key = "menu.feedback.waiting_kcse";
				value.rebuild_requested = true;
			}
		}

		void handle_button(std::string_view button)
		{
			if (button == multiplayer_button)
			{
				kcd2o::account::directory().refresh(
				    kcd2o::ui_settings().account_service_url);
				show_multiplayer_page();
			}
			else if (button == account_button)
			{
				show_account_page();
			}
			else if (button == account_accept_button
			    || button == account_retry_button)
			{
				{
					auto &value = menu_state();
					std::scoped_lock lock(value.mutex);
					value.account_details = false;
				}
				kcd2o::account::service().accept(
				    kcd2o::ui_settings().account_service_url);
				show_account_page();
			}
			else if (button == account_decline_button
			    || button == account_disable_button)
			{
				kcd2o::account::service().decline();
				auto &value = menu_state();
				{
					std::scoped_lock lock(value.mutex);
					value.account_details = false;
					value.pending_join = false;
					value.password.clear();
				}
				show_account_page();
			}
			else if (button == account_continue_button)
			{
				show_multiplayer_page();
			}
			else if (button == account_details_button)
			{
				auto &value = menu_state();
				{
					std::scoped_lock lock(value.mutex);
					value.account_details = !value.account_details;
				}
				show_account_page();
			}
			else if (button == account_copy_button)
			{
				const auto account = kcd2o::account::service().status();
				if (!account.account_id.empty())
					(void)set_clipboard_text(account.account_id);
				show_account_page();
			}
			else if (button == name_button)
			{
				begin_edit(edit_field::name);
			}
			else if (button == password_button)
			{
				begin_edit(edit_field::password);
			}
			else if (button == connect_button)
			{
				connect_or_defer();
			}
			else if (button == refresh_button)
			{
				kcd2o::account::directory().refresh(kcd2o::ui_settings().account_service_url);
				show_multiplayer_page();
			}
			else if (button == favorite_button)
			{
				auto &value = menu_state();
				std::string id;
				{ std::scoped_lock lock(value.mutex); id = value.selected_server_id; }
				if (!id.empty())
					kcd2o::account::directory().set_favorite(id, !kcd2o::account::directory().favorite(id));
				show_multiplayer_page();
			}
			else if (button == browser_back_button)
			{
				auto &value = menu_state();
				{ std::scoped_lock lock(value.mutex); value.selected_server_id.clear(); value.password.clear(); value.editing = edit_field::none; }
				show_multiplayer_page();
			}
			else if (button.starts_with(server_button_prefix))
			{
				const auto id = std::string(button.substr(server_button_prefix.size()));
				auto &value = menu_state();
				{ std::scoped_lock lock(value.mutex); value.selected_server_id = id; value.password = kcd2o::account::directory().password(id).value_or(std::string{}); }
				show_multiplayer_page();
			}
			else if (button == disconnect_button)
			{
				kcd2o::kcse::ui_client().disconnect();
				auto &value = menu_state();
				std::scoped_lock lock(value.mutex);
				value.password.clear();
				value.local_feedback_key.clear();
				value.rebuild_requested = true;
			}
			else if (button == cancel_pending_button)
			{
				auto &value = menu_state();
				std::scoped_lock lock(value.mutex);
				value.pending_join = false;
				value.password.clear();
				value.local_feedback_key.clear();
				value.rebuild_requested = true;
			}
			else if (button == back_button)
			{
				open_root_menu();
			}
			else if (button == respawn_button)
			{
				if (kcd2o::kcse::ui_client().request_respawn())
					show_death_page(true);
			}
		}

		void listener::OnUIEvent(
		    Offsets::IUIElement *,
		    const SUIEventDesc &event,
		    const SUIArguments &arguments,
		    void *)
		{
			const auto *event_name = event.sName
			    ? event.sName
			    : event.sDisplayName;
			if (!event_name
			    || (_stricmp(event_name, "onBasicButton") != 0
			        && _stricmp(event_name, "OnButton") != 0)
			    || arguments.GetArgCount() < 1)
				return;
			const auto button = arguments.GetArg(0).AsString();
			auto &value = menu_state();
			std::scoped_lock lock(value.mutex);
			value.queued_button = button.c_str();
		}

		void listener::OnUnload(Offsets::IUIElement *sender)
		{
			auto &value = menu_state();
			std::scoped_lock lock(value.mutex);
			if (value.element == sender)
			{
				value.element = nullptr;
				value.listener_attached = false;
				value.page_open = false;
				value.account_page_open = false;
				value.death_page_open = false;
				value.account_details = false;
				value.editing = edit_field::none;
			}
		}

		void listener::OnSetVisible(
		    Offsets::IUIElement *sender,
		    bool visible)
		{
			if (visible)
				return;
			auto &value = menu_state();
			std::scoped_lock lock(value.mutex);
			if (value.element == sender)
			{
				value.page_open = false;
				value.account_page_open = false;
				value.death_page_open = false;
				value.account_details = false;
				value.editing = edit_field::none;
			}
		}
	}

	void before_show_page(void *menu) noexcept
	{
		try
		{
			if (!menu)
				return;
			ingame_ui::native_menu_api api(menu);
			if (!api.available())
				return;
			api.clear_information();
			auto *element = api.element();
			auto &value = menu_state();
			bool attach_listener{};
			{
				std::scoped_lock lock(value.mutex);
				value.menu = menu;
				if (value.element != element)
				{
					value.element = element;
					value.listener_attached = false;
				}
				attach_listener = !value.listener_attached;
				value.listener_attached = true;
				value.page_open = false;
				value.account_page_open = false;
				value.editing = edit_field::none;
			}
			if (attach_listener)
				element->AddEventListener(&value.event_listener, "KCD2Online");

			const auto status = kcd2o::kcse::ui_client().status();
			if (api.mode() == 4
			    && status.state == kcd2o::client_state::connected
			    && status.dead)
			{
				show_death_page(false);
				return;
			}

			const auto page = api.current_page();
			if (page != root_main_page && page != root_ingame_page)
				return;
			bool pending{};
			{
				std::scoped_lock lock(value.mutex);
				pending = value.pending_join;
			}
			std::string label;
			if (pending)
				label = ingame_ui::localized("menu.root.pending");
			else if (status.state == kcd2o::client_state::connected)
				label = ingame_ui::localized("menu.root.connected");
			else
			{
				using kcd2o::account::account_state;
				switch (kcd2o::account::service().status().state)
				{
				case account_state::undecided:
					label = ingame_ui::localized("account.root.setup");
					break;
				case account_state::declined:
					label = ingame_ui::localized("account.root.disabled");
					break;
				case account_state::registering:
					label = ingame_ui::localized("account.root.registering");
					break;
				case account_state::error:
					label = ingame_ui::localized("account.root.error");
					break;
				case account_state::ready:
					label = ingame_ui::localized("menu.root.multiplayer");
					break;
				}
			}
			(void)api.append_button(
			    {std::string(multiplayer_button),
			     label,
			     ingame_ui::localized("menu.root.tooltip")});
		}
		catch (...)
		{
			OutputDebugStringA("KCD2Online native menu injection failed.\n");
		}
	}

	void update() noexcept
	{
		try
		{
			auto &value = menu_state();
			const auto current_status = kcd2o::kcse::ui_client().status();
			bool present_error{};
			{
				std::scoped_lock lock(value.mutex);
				if (current_status.state == kcd2o::client_state::disconnected
				    && !current_status.error.empty()
				    && current_status.error != value.presented_error
				    && value.menu)
				{
					ingame_ui::native_menu_api api(value.menu);
					present_error = api.available()
					    && api.mode() == root_main_page;
					if (present_error)
					{
						value.presented_error = current_status.error;
						value.pending_join = false;
						value.local_feedback_key.clear();
						value.editing = edit_field::none;
					}
				}
				else if (current_status.error.empty())
				{
					value.presented_error.clear();
				}
			}
			if (present_error)
				show_multiplayer_page();
			std::string button;
			{
				std::scoped_lock lock(value.mutex);
				button = std::move(value.queued_button);
				value.queued_button.clear();
			}
			if (!button.empty())
				handle_button(button);

			bool death_page_open{};
			{
				std::scoped_lock lock(value.mutex);
				death_page_open = value.death_page_open;
			}
			if (death_page_open)
			{
				const auto status = kcd2o::kcse::ui_client().status();
				if (status.state != kcd2o::client_state::connected
				    || (!status.dead && !status.respawn_pending))
				{
					close_menu();
				}
				return;
			}

			bool account_page_open{};
			std::uint64_t last_account_revision{};
			{
				std::scoped_lock lock(value.mutex);
				account_page_open = value.account_page_open;
				last_account_revision = value.last_account_revision;
			}
			if (account_page_open)
			{
				if (kcd2o::account::service().status().revision
				    != last_account_revision)
					show_account_page();
				return;
			}

			bool pending{};
			{
				std::scoped_lock lock(value.mutex);
				pending = value.pending_join;
			}
			if (pending
			    && kcd2o::account::service().status().multiplayer_enabled()
			    && kcd2o::kcse::ui_client().can_start_join())
			{
				kcd2o::client_options options;
				std::string selected_id;
				{
					std::scoped_lock lock(value.mutex);
					auto &settings = kcd2o::ui_settings();
					options.display_name = settings.display_name;
					options.password = value.password;
					options.account_service_url = settings.account_service_url;
					selected_id = value.selected_server_id;
					value.pending_join = false;
				}
				const auto directory = kcd2o::account::directory().snapshot();
				const auto selected = std::ranges::find_if(directory.servers,
				    [&](const kcd2o::account::public_server &server) { return server.id == selected_id; });
				if (selected != directory.servers.end())
				{
					options.address = selected->address;
					options.server_id = selected->id;
				}
				const auto started =
				    !options.address.empty() && kcd2o::kcse::ui_client().connect(options);
				{
					std::scoped_lock lock(value.mutex);
					if (started)
					{
						value.selected_server_id.clear();
						value.password.clear();
						value.local_feedback_key.clear();
					}
					else
					{
						value.local_feedback_key =
						    "menu.feedback.connect_failed";
					}
					value.rebuild_requested = true;
				}
			}

			bool poll_status{};
			std::uint64_t shown_directory_revision{};
			{
				const auto now = std::chrono::steady_clock::now();
				std::scoped_lock lock(value.mutex);
				poll_status = value.page_open
				    && value.editing == edit_field::none
				    && value.local_feedback_key.empty()
				    && now >= value.next_status_poll;
				if (poll_status)
					value.next_status_poll = now + std::chrono::milliseconds(500);
				shown_directory_revision = value.last_directory_revision;
			}
			if (kcd2o::account::directory().snapshot().revision != shown_directory_revision)
			{
				std::scoped_lock lock(value.mutex);
				if (value.page_open)
					value.rebuild_requested = true;
			}
			if (poll_status)
			{
				const auto status_text = client_state_text(
				    kcd2o::kcse::ui_client().status());
				std::scoped_lock lock(value.mutex);
				if (value.page_open && value.editing == edit_field::none
				    && status_text != value.last_status_text)
					value.rebuild_requested = true;
			}

			bool rebuild{};
			{
				std::scoped_lock lock(value.mutex);
				rebuild = value.rebuild_requested && value.page_open;
			}
			if (rebuild)
				show_multiplayer_page();
		}
		catch (...)
		{
			OutputDebugStringA("KCD2Online native menu update failed.\n");
		}
	}

	bool on_window_message(
	    UINT message,
	    WPARAM wparam,
	    LPARAM) noexcept
	{
		try
		{
			auto &value = menu_state();
			void *account_menu{};
			{
				std::scoped_lock lock(value.mutex);
				if (value.account_page_open)
					account_menu = value.menu;
			}
			if (account_menu)
			{
				int scroll_lines{};
				if (message == WM_MOUSEWHEEL)
				{
					const auto wheel = GET_WHEEL_DELTA_WPARAM(wparam);
					scroll_lines = -(wheel / WHEEL_DELTA) * 3;
				}
				else if (message == WM_KEYDOWN && wparam == VK_PRIOR)
					scroll_lines = -10;
				else if (message == WM_KEYDOWN && wparam == VK_NEXT)
					scroll_lines = 10;
				if (scroll_lines != 0)
					return ingame_ui::native_menu_api(account_menu)
					    .scroll_information(scroll_lines);
			}

			std::scoped_lock lock(value.mutex);
			if (!value.page_open || value.editing == edit_field::none)
				return false;

			auto &text = edited_value(value);
			const auto limit = value.editing == edit_field::password
			    ? kcd2o::kcse::text_capacity - 1
			    : kcd2o::kcse::short_text_capacity - 1;
			if (message == WM_KEYDOWN && wparam == VK_ESCAPE)
			{
				text = value.edit_original;
				value.editing = edit_field::none;
				value.rebuild_requested = true;
				return true;
			}
			if (message == WM_KEYDOWN && wparam == VK_RETURN)
			{
				const auto field = value.editing;
				value.editing = edit_field::none;
				value.rebuild_requested = true;
				persist_edit(field);
				return true;
			}
			if (message == WM_KEYDOWN && wparam == VK_BACK)
			{
				erase_last_utf8_character(text);
				value.rebuild_requested = true;
				return true;
			}
			if (message == WM_KEYDOWN && wparam == 'V'
			    && (GetKeyState(VK_CONTROL) & 0x8000) != 0)
			{
				auto pasted = clipboard_text();
				pasted.erase(
				    std::remove_if(
				        pasted.begin(),
				        pasted.end(),
				        [](unsigned char character)
				        {
					        return character == '\r' || character == '\n'
					            || character == '\t';
				        }),
				    pasted.end());
				if (text.size() < limit)
					text.append(utf8_prefix(pasted, limit - text.size()));
				value.rebuild_requested = true;
				return true;
			}
			if (message != WM_CHAR || wparam < 0x20 || wparam == 0x7F)
				return message == WM_KEYDOWN;

			const wchar_t character = static_cast<wchar_t>(wparam);
			auto encoded = narrow_utf8(std::wstring_view(&character, 1));
			if (text.size() + encoded.size() <= limit)
				text += encoded;
			value.rebuild_requested = true;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	bool blocks_game_input() noexcept
	{
		auto &value = menu_state();
		std::scoped_lock lock(value.mutex);
		return value.page_open && value.editing != edit_field::none;
	}
}
