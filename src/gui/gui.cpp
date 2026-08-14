#include "gui.hpp"

#include "gui/ingame_chat.hpp"
#include "gui/ingame_player_hub.hpp"
#include "gui/ingame_social_panel.hpp"
#include "gui/ingame_staff_panel.hpp"
#include "gui/native_multiplayer_menu.hpp"
#include "gui/renderer.hpp"
#include "hooks/hooking.hpp"
#include "kcd2_address.hpp"
#include "kcse/client_proxy.hpp"
#include "lua/bindings/imgui_window.hpp"
#include "lua_extensions/lua_manager_extension.hpp"
#include "lua_extensions/lua_module_ext.hpp"
#include "multiplayer/ui_settings.hpp"
#include "npc/catalog.hpp"

#include <gui/widgets/imgui_extensions.hpp>
#include <gui/widgets/imgui_hotkey.hpp>
#include <input/hotkey.hpp>
#include <input/is_key_pressed.hpp>
#include <kcd2_init.hpp>
#include <lua/lua_manager.hpp>
#include <memory/gm_address.hpp>
#include <misc/cpp/imgui_stdlib.h>

namespace big
{
	extern toml_v2::config_file::config_entry<bool>* g_hook_log_write_enabled;

	namespace
	{
		// The player-facing connection flow lives in KCD2's native Scaleform
		// menu. Keep the ImGui panel available as an explicit developer fallback,
		// but do not open it by default.
		bool g_show_multiplayer = false;
		bool g_show_developer_console = false;

		struct developer_console_entry
		{
			std::string command;
			engine_console_submit_status status{
			    engine_console_submit_status::queued};
		};

		struct developer_console_state
		{
			std::string input;
			std::vector<std::string> command_history;
			std::deque<developer_console_entry> entries;
			int history_position{-1};
			bool scroll_to_bottom{};
		};

		const char *console_status_text(engine_console_submit_status status)
		{
			switch (status)
			{
			case engine_console_submit_status::queued:
				return "queued";
			case engine_console_submit_status::unavailable:
				return "rejected: retail console unavailable";
			case engine_console_submit_status::empty:
				return "rejected: command is empty";
			case engine_console_submit_status::too_long:
				return "rejected: command exceeds 1024 characters";
			case engine_console_submit_status::full:
				return "rejected: command queue is full";
			}
			return "rejected";
		}

		int DeveloperConsoleHistoryCallback(
		    ImGuiInputTextCallbackData *data)
		{
			auto &state =
			    *static_cast<developer_console_state *>(data->UserData);
			if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory
			    || state.command_history.empty())
			{
				return 0;
			}

			if (data->EventKey == ImGuiKey_UpArrow)
			{
				if (state.history_position < 0)
				{
					state.history_position =
					    static_cast<int>(state.command_history.size()) - 1;
				}
				else if (state.history_position > 0)
				{
					--state.history_position;
				}
			}
			else if (data->EventKey == ImGuiKey_DownArrow
			         && state.history_position >= 0)
			{
				++state.history_position;
				if (state.history_position
				    >= static_cast<int>(state.command_history.size()))
				{
					state.history_position = -1;
				}
			}

			const auto command = state.history_position >= 0
			    ? state.command_history[static_cast<std::size_t>(
			          state.history_position)]
			    : std::string{};
			data->DeleteChars(0, data->BufTextLen);
			data->InsertChars(0, command.c_str());
			return 0;
		}

		void RenderDeveloperConsole()
		{
			if (!g_show_developer_console)
			{
				return;
			}

			static developer_console_state state;
			ImGui::SetNextWindowSize({700.0F, 360.0F}, ImGuiCond_FirstUseEver);
			if (!ImGui::Begin(
			        "KCD2Online Developer Console",
			        &g_show_developer_console))
			{
				ImGui::End();
				return;
			}

			ImGui::TextColored(
			    ImVec4(1.0F, 0.72F, 0.2F, 1.0F),
			    "Local developer access: commands may alter or unload the active game.");
			ImGui::TextWrapped(
			    "Commands are queued locally and executed on the game thread. "
			    "They are never sent by or exposed to the multiplayer server.");

			const bool console_available = engine_console_available();
			ImGui::Text(
			    "Retail console: %s",
			    console_available ? "available" : "unavailable");
			ImGui::SameLine();
			if (ImGui::Button("Clear"))
			{
				state.entries.clear();
				state.command_history.clear();
				state.history_position = -1;
			}

			if (ImGui::BeginChild(
			        "DeveloperConsoleHistory",
			        {0.0F, -ImGui::GetFrameHeightWithSpacing() * 2.0F},
			        true))
			{
				for (const auto &entry : state.entries)
				{
					ImGui::TextUnformatted(("> " + entry.command).c_str());
					const auto color =
					    entry.status == engine_console_submit_status::queued
					    ? ImVec4(0.45F, 0.85F, 0.45F, 1.0F)
					    : ImVec4(1.0F, 0.35F, 0.25F, 1.0F);
					ImGui::TextColored(
					    color,
					    "  %s",
					    console_status_text(entry.status));
				}
				if (state.scroll_to_bottom)
				{
					ImGui::SetScrollHereY(1.0F);
					state.scroll_to_bottom = false;
				}
			}
			ImGui::EndChild();

			ImGui::BeginDisabled(!console_available);
			ImGui::SetNextItemWidth(-90.0F);
			const auto input_flags =
			    ImGuiInputTextFlags_EnterReturnsTrue
			    | ImGuiInputTextFlags_CallbackHistory;
			const bool submitted_with_enter = ImGui::InputText(
			    "##DeveloperConsoleInput",
			    &state.input,
			    input_flags,
			    DeveloperConsoleHistoryCallback,
			    &state);
			ImGui::SameLine();
			const bool submitted_with_button = ImGui::Button("Execute");
			ImGui::EndDisabled();

			if (submitted_with_enter || submitted_with_button)
			{
				const auto command = state.input;
				const auto status =
				    queue_engine_console_command(command);
				state.entries.push_back({
				    command.empty() ? "<empty>" : command,
				    status});
				while (state.entries.size() > 100)
				{
					state.entries.pop_front();
				}
				if (status == engine_console_submit_status::queued)
				{
					state.command_history.push_back(command);
					while (state.command_history.size() > 100)
					{
						state.command_history.erase(
						    state.command_history.begin());
					}
					state.input.clear();
				}
				state.history_position = -1;
				state.scroll_to_bottom = true;
			}

			ImGui::TextDisabled(
			    "%zu / 1024 characters | Up/Down: command history",
			    state.input.size());
			ImGui::End();
		}

		void RenderMultiplayer()
		{
			auto &client = kcd2o::kcse::ui_client();
			if (!g_show_multiplayer)
			{
				return;
			}

			static auto* saved_diagnostic_logging = config::general().bind(
			    "Multiplayer",
			    "Diagnostic Logging",
			    false,
			    "Write verbose KCD2Online join and performance diagnostics.");
			auto& preferences = kcd2o::ui_settings();
			auto& address = preferences.address;
			auto& display_name = preferences.display_name;
			bool diagnostic_logging =
			    saved_diagnostic_logging->get_value();
			client.set_diagnostic_logging(diagnostic_logging);
			static std::string password;
			static std::string claim_code;
			static std::string chat_text;
			static std::string npc_catalog_filter;

			ImGui::SetNextWindowSize({520.0F, 560.0F}, ImGuiCond_FirstUseEver);
			if (!ImGui::Begin("KCD2Online Multiplayer", &g_show_multiplayer))
			{
				ImGui::End();
				return;
			}

			auto status = client.status();
			ImGui::Text("Status: %s", kcd2o::to_string(status.state));
			if (!status.server_name.empty())
			{
				ImGui::SameLine();
				ImGui::TextDisabled("(%s)", status.server_name.c_str());
			}
			const auto sandbox = client.runtime_capability();
			const auto joinable = client.can_start_join();
			const bool disconnected =
			    status.state == kcd2o::client_state::disconnected;
			ImGui::Text(
			    "Multiplayer runtime: %s",
			    sandbox.available ? "ready" :
			                        (disconnected ? "idle" : "initializing"));
			if (!sandbox.available)
			{
				ImGui::TextWrapped("%s", sandbox.diagnostic.c_str());
			}
			const auto loaded_level = client.current_level_id();
			if (!loaded_level.empty())
			{
				ImGui::Text(
				    "Join source: loaded native save (level %s)",
				    loaded_level.c_str());
				ImGui::TextWrapped(
				    "No savegame is required. The server bootstrap starts the native target world; save and autosave are locked while connected.");
			}
			else
			{
				ImGui::TextUnformatted(
				    "Join source: load a native save before connecting");
			}
			if (status.ping_ms >= 0)
			{
				ImGui::Text(
				    "Ping: %d ms | Loss: %.1f%% | Queue: %zu",
				    status.ping_ms,
				    status.packet_loss_percent,
				    status.game_queue_size);
			}
			if (!status.error.empty())
			{
				ImGui::TextColored(
				    ImVec4(1.0F, 0.35F, 0.25F, 1.0F),
				    "%s",
				    status.error.c_str());
			}
			if (ImGui::Checkbox(
			        "Diagnostic logging",
			        &diagnostic_logging))
			{
				saved_diagnostic_logging->set_value(diagnostic_logging);
				client.set_diagnostic_logging(diagnostic_logging);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip(
				    "Writes verbose join and performance data to "
				    "KCD2Online-join.log. Native crash records remain enabled.");
			}

			ImGui::BeginDisabled(!disconnected);
			if (ImGui::InputText("Address", &address))
			{
				preferences.persist_address();
			}
			if (ImGui::InputText("Name", &display_name))
			{
				preferences.persist_display_name();
			}
			ImGui::InputText(
			    "Password",
			    &password,
			    ImGuiInputTextFlags_Password);
			ImGui::InputText("Recovery claim code", &claim_code);
			ImGui::EndDisabled();

			ImGui::BeginDisabled(!disconnected || !joinable);
			if (ImGui::Button("Connect"))
			{
				kcd2o::client_options options;
				options.address = address;
				options.display_name = display_name;
				options.password = password;
				options.claim_code = claim_code;
				if (!client.connect(options))
				{
					LOG(WARNING) << "Multiplayer connect request was invalid.";
				}
				password.clear();
				claim_code.clear();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(disconnected);
			if (ImGui::Button("Disconnect"))
			{
				client.disconnect();
				password.clear();
			}
			ImGui::EndDisabled();

			ImGui::SeparatorText("Players");
			const auto players = client.players();
			ImGui::Text(
			    "You: %llu | Players: %zu",
			    static_cast<unsigned long long>(status.local_player_id),
			    players.size());
			if (status.state == kcd2o::client_state::connected
			    && status.avatar_policy.allowed_archetype_ids_size() != 0)
			{
				const char* selected_id = status.avatar_archetype_id.empty()
				    ? status.avatar_policy.default_archetype_id().c_str()
				    : status.avatar_archetype_id.c_str();
				const auto *selected_entry =
				    kcd2o::npc::runtime_catalog().find(selected_id);
				const auto selected_label = selected_entry
				    ? std::format(
				          "{} ({})",
				          selected_entry->soul_name,
				          selected_entry->character_id)
				    : std::string(selected_id);
				if (ImGui::BeginCombo(
				        "Player NPC model",
				        selected_label.c_str()))
				{
					for (const auto& archetype :
					     status.avatar_policy.allowed_archetype_ids())
					{
						const bool active =
						    archetype == status.avatar_archetype_id;
						const auto *entry =
						    kcd2o::npc::runtime_catalog().find(archetype);
						const auto label = entry
						    ? std::format(
						          "{} ({})##{}",
						          entry->soul_name,
						          entry->character_id,
						          archetype)
						    : archetype;
						if (ImGui::Selectable(label.c_str(), active))
						{
							(void)client.select_avatar(archetype);
						}
						if (ImGui::IsItemHovered())
						{
							ImGui::SetTooltip("%s", archetype.c_str());
						}
						if (active)
						{
							ImGui::SetItemDefaultFocus();
						}
					}
					ImGui::EndCombo();
				}
				ImGui::TextDisabled("Soul UUID: %s", selected_id);
				ImGui::SameLine();
				if (ImGui::SmallButton("Copy##SelectedSoul"))
					ImGui::SetClipboardText(selected_id);
			}
			if (ImGui::CollapsingHeader("NPC catalog"))
			{
				const auto &catalog = kcd2o::npc::runtime_catalog();
				ImGui::InputTextWithHint(
				    "##NpcCatalogFilter",
				    "Search name, Character-ID or Soul UUID",
				    &npc_catalog_filter);
				ImGui::TextDisabled(
				    "%zu human Souls from local Tables.pak",
				    catalog.size());
				if (ImGui::BeginChild(
				        "NpcCatalogResults",
				        {0.0F, 180.0F},
				        true))
				{
					std::string filter = npc_catalog_filter;
					std::ranges::transform(
					    filter,
					    filter.begin(),
					    [](unsigned char value)
					    {
						    return static_cast<char>(std::tolower(value));
					    });
					std::size_t shown{};
					for (const auto &entry : catalog.entries())
					{
						auto searchable = std::format(
						    "{} {} {}",
						    entry.soul_name,
						    entry.character_id,
						    entry.soul_id);
						std::ranges::transform(
						    searchable,
						    searchable.begin(),
						    [](unsigned char value)
						    {
							    return static_cast<char>(std::tolower(value));
						    });
						if (!filter.empty()
						    && !searchable.contains(filter))
							continue;
						ImGui::PushID(entry.soul_id.c_str());
						ImGui::TextUnformatted(entry.soul_name.c_str());
						ImGui::TextDisabled(
						    "%s | %s",
						    entry.character_id.c_str(),
						    entry.soul_id.c_str());
						ImGui::SameLine();
						if (ImGui::SmallButton("Copy"))
							ImGui::SetClipboardText(entry.soul_id.c_str());
						ImGui::PopID();
						if (++shown >= 200)
						{
							ImGui::TextDisabled(
							    "Showing first 200 matches; refine the search.");
							break;
						}
					}
				}
				ImGui::EndChild();
			}
			if (ImGui::BeginTable(
			        "MultiplayerPlayers",
			        4,
			        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
			{
				ImGui::TableSetupColumn("ID");
				ImGui::TableSetupColumn("Name");
				ImGui::TableSetupColumn("State");
				ImGui::TableSetupColumn("Movement");
				ImGui::TableHeadersRow();
				for (const auto& player : players)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::Text("%llu", static_cast<unsigned long long>(player.id));
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(player.display_name.c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(player.connected ? "online" : "reconnecting");
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(
					    kcd2o::protocol::MovementMode_Name(player.movement_mode).c_str());
				}
				ImGui::EndTable();
			}

			ImGui::SeparatorText("Global Chat");
			if (ImGui::BeginChild("MultiplayerChat", {0.0F, 150.0F}, true))
			{
				for (const auto& entry : client.chat_history())
				{
					ImGui::TextWrapped(
					    "%s: %s",
					    entry.display_name.c_str(),
					    entry.text.c_str());
				}
			}
			ImGui::EndChild();
			ImGui::BeginDisabled(
			    status.state != kcd2o::client_state::connected);
			const bool submit = ImGui::InputText(
			    "##ChatInput",
			    &chat_text,
			    ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::SameLine();
			if ((submit || ImGui::Button("Send")) && !chat_text.empty())
			{
				if (client.send_chat(chat_text))
				{
					chat_text.clear();
				}
			}
			ImGui::EndDisabled();
			ImGui::End();
		}
	}

	gui::gui()
	{
		init_pref();

		g_renderer->add_dx_callback({[this]
		                             {
			                             dx_on_tick();
		                             },
		                             -5});

		g_renderer->add_wndproc_callback(
		    [this](HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
		    {
			    wndproc(hwnd, msg, wparam, lparam);
		    });

		g_renderer->add_init_callback(
		    [this]()
		    {
			    dx_init();
			    //g_renderer->rescale(g_gui->scale);
		    });

		g_gui = this;
	}

	gui::~gui()
	{
		g_gui = nullptr;
	}

	bool gui::is_open()
	{
		return m_is_open;
	}

	void gui::toggle(bool toggle)
	{
		m_is_open = toggle;

		sync_mouse_capture();
	}

	void gui::sync_mouse_capture()
	{
		toggle_mouse();
	}

	void gui::dx_init()
	{
		static auto bgColor     = ImVec4(0.09f, 0.094f, 0.129f, .9f);
		static auto primary     = ImVec4(0.172f, 0.380f, 0.909f, 1.f);
		static auto secondary   = ImVec4(0.443f, 0.654f, 0.819f, 1.f);
		static auto whiteBroken = ImVec4(0.792f, 0.784f, 0.827f, 1.f);

		auto& style             = ImGui::GetStyle();
		style.WindowPadding     = ImVec2(15, 15);
		style.WindowRounding    = 10.f;
		style.WindowBorderSize  = 0.f;
		style.FramePadding      = ImVec2(5, 5);
		style.FrameRounding     = 4.0f;
		style.ItemSpacing       = ImVec2(12, 8);
		style.ItemInnerSpacing  = ImVec2(8, 6);
		style.IndentSpacing     = 25.0f;
		style.ScrollbarSize     = 15.0f;
		style.ScrollbarRounding = 9.0f;
		style.GrabMinSize       = 5.0f;
		style.GrabRounding      = 3.0f;
		style.ChildRounding     = 4.0f;

		auto& colors = style.Colors;
		//colors[ImGuiCol_Text]                 = ImGui::ColorConvertU32ToFloat4(g_gui->text_color);
		colors[ImGuiCol_TextDisabled] = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
		//colors[ImGuiCol_WindowBg]             = ImGui::ColorConvertU32ToFloat4(g_gui->background_color);
		//colors[ImGuiCol_ChildBg]              = ImGui::ColorConvertU32ToFloat4(g_gui->background_color);
		colors[ImGuiCol_PopupBg]              = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
		colors[ImGuiCol_Border]               = ImVec4(0.80f, 0.80f, 0.83f, 0.88f);
		colors[ImGuiCol_BorderShadow]         = ImVec4(0.92f, 0.91f, 0.88f, 0.00f);
		colors[ImGuiCol_FrameBg]              = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
		colors[ImGuiCol_FrameBgActive]        = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		colors[ImGuiCol_TitleBg]              = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		colors[ImGuiCol_TitleBgCollapsed]     = ImVec4(1.00f, 0.98f, 0.95f, 0.75f);
		colors[ImGuiCol_TitleBgActive]        = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
		colors[ImGuiCol_MenuBarBg]            = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
		colors[ImGuiCol_ScrollbarGrab]        = ImVec4(0.80f, 0.80f, 0.83f, 0.31f);
		colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		colors[ImGuiCol_CheckMark]            = ImVec4(1.00f, 0.98f, 0.95f, 0.61f);
		colors[ImGuiCol_SliderGrab]           = ImVec4(0.80f, 0.80f, 0.83f, 0.31f);
		colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		colors[ImGuiCol_Button]               = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
		colors[ImGuiCol_ButtonHovered]        = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
		colors[ImGuiCol_ButtonActive]         = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		colors[ImGuiCol_Header]               = ImVec4(0.30f, 0.29f, 0.32f, 1.00f);
		colors[ImGuiCol_HeaderHovered]        = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		colors[ImGuiCol_HeaderActive]         = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		colors[ImGuiCol_ResizeGrip]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
		colors[ImGuiCol_ResizeGripHovered]    = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
		colors[ImGuiCol_ResizeGripActive]     = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
		colors[ImGuiCol_PlotLines]            = ImVec4(0.40f, 0.39f, 0.38f, 0.63f);
		colors[ImGuiCol_PlotLinesHovered]     = ImVec4(0.25f, 1.00f, 0.00f, 1.00f);
		colors[ImGuiCol_PlotHistogram]        = ImVec4(0.40f, 0.39f, 0.38f, 0.63f);
		colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.25f, 1.00f, 0.00f, 1.00f);
		colors[ImGuiCol_TextSelectedBg]       = ImVec4(0.25f, 1.00f, 0.00f, 0.43f);

		save_default_style();
	}

	static bool editing_gui_keybind = false;

	static __int64 C_PlayerStateMovement_Fly_callback(__int64 C_PlayerStateMovement, __int64 out_struct_40bytesize, __int64 C_Player, __int64 state_value)
	{
	}

	void RenderEntityXmlInfo(entity_xml_info_t* current, const std::string& entity_class_name);

	void RenderEntityDetails(int selected_index)
	{
		ImGui::Begin("Entity Details", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

		ImGui::Text("Runtime Info");

		const auto& entity = g_entities[selected_index];
		const auto& e      = g_entity_infos[entity];

		ImGui::Text("Name: %s", e.name.c_str());
		ImGui::Text("Class: %s", e.class_name.c_str());
		ImGui::Text("Archetype: %s", e.archetype_name.c_str());
		ImGui::Text("ID: %u (Mask: %u) (Salt: %u)", e.id, e.id_mask, e.id_salt);

		Vec3 position;
		entity->GetPos(position);
		ImGui::Text("Position: (%.2f, %.2f, %.2f)", position.x, position.y, position.z);
		if (ImGui::Button("Teleport To Position"))
		{
			g_player_entity->SetWorldPos(position);

			//const auto formatted_str =
			//std::vformat("if rom and rom.game then "
			//"rom.game.player:SetWorldPos({{x={:.2f},y={:.2f},z={:.2f}}}) else player:"
			//"SetWorldPos({{x={:.2f},y={:.2f},z={:.2f}}}) end",
			//std::make_format_args(position[0], position[1], position[2], position[0], position[1], position[2]));


			//g_lua_execute_buffer_queue.push_back(formatted_str);
		}

		ImGui::Text("Active: %s", entity->IsActive() ? "Yes" : "No");
		ImGui::Text("Hidden: %s", entity->IsHidden() ? "Yes" : "No");
		if (e.layer_name.size())
		{
			ImGui::Text("Layer: %s (ID: %u)", e.layer_name.c_str(), e.layer_id);
		}
		else
		{
			ImGui::Text("Layer: None");
		}
		ImGui::Text("GUID: %s", e.guid.c_str());

		auto* xml_info = g_entity_guid_to_xml_infos[e.guid];
		if (xml_info)
		{
			ImGui::Separator();

			ImGui::Text("XML On-Disk Info");

			RenderEntityXmlInfo(xml_info, xml_info->m_entity_class_name);
		}

		ImGui::End();
	}

	void RenderEntityInspectorTable()
	{
		static char search_buffer[256]  = "";
		static char search_buffer2[256] = "";
		auto callback_search            = []()
		{
			memcpy(search_buffer, search_buffer2, 256);
			// Convert the search buffer to lowercase
			for (int i = 0; search_buffer2[i]; i++)
			{
				search_buffer[i] = tolower((unsigned char)search_buffer2[i]);
			}

			g_entities_filtered.clear();

			size_t i = 0;
			for (auto& current_entity : g_entities)
			{
				const auto& current_entity_info = g_entity_infos[current_entity];
				if (current_entity_info.name_lower.find(search_buffer) != std::string::npos
				    || current_entity_info.class_name_lower.find(search_buffer) != std::string::npos
				    || current_entity_info.id_string.find(search_buffer) != std::string::npos
				    || current_entity_info.layer_name_lower.find(search_buffer) != std::string::npos
				    || current_entity_info.guid.find(search_buffer) != std::string::npos)
				{
					g_entities_filtered.push_back(i);
				}

				i++;
			}
		};
		if (ImGui::InputText("##SearchInput", search_buffer2, IM_ARRAYSIZE(search_buffer2)))
		{
			callback_search();
		}
		ImGui::SameLine();
		if (ImGui::Button("Clear Search"))
		{
			memset(search_buffer, 0, 256);
			memset(search_buffer2, 0, 256);

			g_entities_filtered.clear();
		}

		if (ImGui::BeginTable("EntityTable", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable))
		{
			// Table Setup
			ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("GUID", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Active", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Hidden", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			// Sorting and Table Display Logic (same as before)
			ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
			if (sortSpecs->SpecsDirty)
			{
				if (sortSpecs->SpecsCount > 0)
				{
					const ImGuiTableColumnSortSpecs& specs = sortSpecs->Specs[0];
					if (specs.ColumnIndex == 0)
					{
						if (specs.SortDirection == ImGuiSortDirection_Ascending)
						{
							std::sort(g_entities.begin(),
							          g_entities.end(),
							          [](CEntity* a_, CEntity* b_)
							          {
								          const auto& a = g_entity_infos[a_];
								          const auto& b = g_entity_infos[b_];

								          return a.id < b.id;
							          });
						}
						else
						{
							std::sort(g_entities.begin(),
							          g_entities.end(),
							          [](CEntity* a_, CEntity* b_)
							          {
								          const auto& a = g_entity_infos[a_];
								          const auto& b = g_entity_infos[b_];

								          return a.id > b.id;
							          });
						}
					}
					else if (specs.ColumnIndex == 1)
					{
						if (specs.SortDirection == ImGuiSortDirection_Ascending)
						{
							std::sort(g_entities.begin(),
							          g_entities.end(),
							          [](CEntity* a_, CEntity* b_)
							          {
								          const auto& a = g_entity_infos[a_];
								          const auto& b = g_entity_infos[b_];

								          return a.name < b.name;
							          });
						}
						else
						{
							std::sort(g_entities.begin(),
							          g_entities.end(),
							          [](CEntity* a_, CEntity* b_)
							          {
								          const auto& a = g_entity_infos[a_];
								          const auto& b = g_entity_infos[b_];

								          return a.name > b.name;
							          });
						}
					}
					else if (specs.ColumnIndex == 2)
					{
						if (specs.SortDirection == ImGuiSortDirection_Ascending)
						{
							std::sort(g_entities.begin(),
							          g_entities.end(),
							          [](CEntity* a_, CEntity* b_)
							          {
								          const auto& a = g_entity_infos[a_];
								          const auto& b = g_entity_infos[b_];

								          return a.guid < b.guid;
							          });
						}
						else
						{
							std::sort(g_entities.begin(),
							          g_entities.end(),
							          [](CEntity* a_, CEntity* b_)
							          {
								          const auto& a = g_entity_infos[a_];
								          const auto& b = g_entity_infos[b_];

								          return a.guid > b.guid;
							          });
						}
					}
					else if (specs.ColumnIndex == 3)
					{
						if (specs.SortDirection == ImGuiSortDirection_Ascending)
						{
							std::sort(g_entities.begin(),
							          g_entities.end(),
							          [](CEntity* a_, CEntity* b_)
							          {
								          return a_->IsActive() < b_->IsActive();
							          });
						}
						else
						{
							std::sort(g_entities.begin(),
							          g_entities.end(),
							          [](CEntity* a_, CEntity* b_)
							          {
								          return a_->IsActive() > b_->IsActive();
							          });
						}
					}
					else if (specs.ColumnIndex == 4)
					{
						if (specs.SortDirection == ImGuiSortDirection_Ascending)
						{
							std::sort(g_entities.begin(),
							          g_entities.end(),
							          [](CEntity* a_, CEntity* b_)
							          {
								          return a_->IsHidden() < b_->IsHidden();
							          });
						}
						else
						{
							std::sort(g_entities.begin(),
							          g_entities.end(),
							          [](CEntity* a_, CEntity* b_)
							          {
								          return a_->IsHidden() > b_->IsHidden();
							          });
						}
					}
					else if (specs.ColumnIndex == 5)
					{
						if (specs.SortDirection == ImGuiSortDirection_Ascending)
						{
							std::sort(g_entities.begin(),
							          g_entities.end(),
							          [](CEntity* a_, CEntity* b_)
							          {
								          const auto& a = g_entity_infos[a_];
								          const auto& b = g_entity_infos[b_];

								          return a.class_name < b.class_name;
							          });
						}
						else
						{
							std::sort(g_entities.begin(),
							          g_entities.end(),
							          [](CEntity* a_, CEntity* b_)
							          {
								          const auto& a = g_entity_infos[a_];
								          const auto& b = g_entity_infos[b_];

								          return a.class_name > b.class_name;
							          });
						}
					}
				}
				sortSpecs->SpecsDirty = false;
			}

			ImGuiListClipper clipper;
			clipper.Begin(g_entities_filtered.size() ? g_entities_filtered.size() : g_entities.size());

			while (clipper.Step())
			{
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
				{
					const auto entity_index         = g_entities_filtered.size() ? g_entities_filtered[i] : i;
					const auto& current_entity      = g_entities[entity_index];
					const auto& current_entity_info = g_entity_infos[current_entity];

					ImGui::TableNextRow();

					// ID column
					ImGui::TableSetColumnIndex(0);
					char idBuffer[16];
					snprintf(idBuffer, sizeof(idBuffer), "%u", current_entity_info.id);
					if (ImGui::Selectable(idBuffer, g_selected_index_entity_detail_inspector == i, ImGuiSelectableFlags_SpanAllColumns))
					{
						g_selected_index_entity_detail_inspector = entity_index;
					}

					// Name column
					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted(current_entity_info.name.c_str());

					// GUID column
					ImGui::TableSetColumnIndex(2);
					ImGui::TextUnformatted(current_entity_info.guid.c_str());

					// Active column
					ImGui::TableSetColumnIndex(3);
					ImGui::TextUnformatted(current_entity->IsActive() ? "Yes" : "No");

					// Hidden column
					ImGui::TableSetColumnIndex(4);
					ImGui::TextUnformatted(current_entity->IsHidden() ? "Yes" : "No");

					// Class column
					ImGui::TableSetColumnIndex(5);
					ImGui::TextUnformatted(current_entity_info.class_name.c_str());

					// Layer column
					ImGui::TableSetColumnIndex(6);
					ImGui::TextUnformatted(current_entity_info.layer_name.c_str());
				}
			}

			ImGui::EndTable();
		}

		// Render the separate entity details window if an entity is selected
		if (g_selected_index_entity_detail_inspector >= 0 && g_selected_index_entity_detail_inspector < (int)g_entities.size())
		{
			RenderEntityDetails(g_selected_index_entity_detail_inspector);
		}
	}

	void RenderCBrushDetail()
	{
		ImGui::Begin("CBrush Details", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

		if (g_selected_cbrush_detail_inspector < 0
		    || static_cast<size_t>(g_selected_cbrush_detail_inspector) >= g_cbrushes.size())
		{
			ImGui::Text("No brush selected.");
			ImGui::End();
			return;
		}

		const auto& brush = g_cbrushes[g_selected_cbrush_detail_inspector];

		ImGui::Text("Name: %s", brush->GetName());

		const auto position = brush->GetPos();
		ImGui::Text("Position: (%.2f, %.2f, %.2f)", position.x, position.y, position.z);
		if (ImGui::Button("Teleport To Position"))
		{
			g_player_entity->SetWorldPos(position);
		}

		static float new_pos[3] = {position.x, position.y, position.z};

		static int last_selected_brush_index = -1;
		// Reset new_pos if the selected brush changes
		if (last_selected_brush_index != g_selected_cbrush_detail_inspector)
		{
			new_pos[0]                = position.x;
			new_pos[1]                = position.y;
			new_pos[2]                = position.z;
			last_selected_brush_index = g_selected_cbrush_detail_inspector;
		}

		ImGui::Text("Matrix Manipulation Mode:");
		ImGui::RadioButton("Local", &g_imguizmo_editor_local_or_world, ImGuizmo::LOCAL);
		ImGui::SameLine();
		ImGui::RadioButton("World", &g_imguizmo_editor_local_or_world, ImGuizmo::WORLD);

		ImGui::Text("New Position:");
		ImGui::InputFloat3("##position", new_pos);
		if (ImGui::Button("Validate Position"))
		{
			float new_matrix[3 * 4];
			memcpy(new_matrix, brush->m_Matrix, sizeof(new_matrix));
			new_matrix[3]  = new_pos[0];
			new_matrix[7]  = new_pos[1];
			new_matrix[11] = new_pos[2];

			brush->SetMatrix(new_matrix);
		}

		if (ImGui::Button("Clone"))
		{
			auto brush_cloned = brush->Clone();

			// SetMatrix needed otherwise the cloned brush will be invisible for some reason?
			brush_cloned->SetMatrix(brush->m_Matrix);
		}

		ImGui::Text("CStatObj Index:");
		ImGui::SameLine();
		static int cstat_obj_index = 0;
		ImGui::InputInt("##CStatObjIndex", &cstat_obj_index);
		if (cstat_obj_index < 0)
		{
			cstat_obj_index = 0;
		}
		else if (cstat_obj_index >= g_cstatobjs.size())
		{
			cstat_obj_index = g_cstatobjs.size() - 1;
		}

		/*if (ImGui::Button("SetStatObj Test"))
		{
			g_CBrush_SetStatObj(brush, (void*)g_cstatobjs[cstat_obj_index]);
		}*/

		ImGui::End();
	}

	void RenderCBrushInspector()
	{
		if (!g_show_cbrush_inspector->get_value())
		{
			return;
		}

		ImGui::ConfigBind(ImGui::Begin, "CBrush Inspector", g_show_cbrush_inspector, 0);

		ImGui::Text("CBrush Count: %llu", g_cbrushes.size());
		ImGui::Separator();

		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(g_cbrushes.size()));

		if (ImGui::BeginTable("CBrushTable", 1, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
		{
			size_t push_id = 0;
			while (clipper.Step())
			{
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
				{
					ImGui::PushID(push_id);

					auto* cbrush = g_cbrushes[i];
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					auto brush_name = cbrush->GetName();
					if (ImGui::Selectable(strlen(brush_name) ? brush_name : "No Name", g_selected_cbrush_detail_inspector == i))
					{
						g_selected_cbrush_detail_inspector = i;
					}

					ImGui::PopID();
					push_id++;
				}
			}
			ImGui::EndTable();
		}

		RenderCBrushDetail();

		ImGui::End();
	}

	void RenderCMergedMeshRenderNodeDetail()
	{
		ImGui::Begin("CMergedMeshRenderNode Details", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

		if (g_selected_CMergedMeshRenderNode_detail_inspector < 0
		    || static_cast<size_t>(g_selected_CMergedMeshRenderNode_detail_inspector) >= g_CMergedMeshRenderNodes.size())
		{
			ImGui::Text("No CMergedMeshRenderNode selected.");
			ImGui::End();
			return;
		}

		const auto& obj = g_CMergedMeshRenderNodes[g_selected_CMergedMeshRenderNode_detail_inspector];

		ImGui::Text("Name: %s", obj->GetName());

		const auto position = obj->GetPos();
		ImGui::Text("Position: (%.2f, %.2f, %.2f)", position.x, position.y, position.z);
		if (ImGui::Button("Teleport To Position"))
		{
			g_player_entity->SetWorldPos(position);
		}

		static float new_pos[3] = {position.x, position.y, position.z};

		static int last_selected_obj_index = -1;
		// Reset new_pos if the selected obj changes
		if (last_selected_obj_index != g_selected_CMergedMeshRenderNode_detail_inspector)
		{
			new_pos[0]              = position.x;
			new_pos[1]              = position.y;
			new_pos[2]              = position.z;
			last_selected_obj_index = g_selected_CMergedMeshRenderNode_detail_inspector;
		}

		ImGui::Text("Matrix Manipulation Mode:");
		ImGui::RadioButton("Local", &g_imguizmo_editor_local_or_world, ImGuizmo::LOCAL);
		ImGui::SameLine();
		ImGui::RadioButton("World", &g_imguizmo_editor_local_or_world, ImGuizmo::WORLD);

		ImGui::End();
	}

	void RenderCMergedMeshRenderNodeInspector()
	{
		if (!g_show_CMergedMeshRenderNode_inspector->get_value())
		{
			return;
		}

		ImGui::ConfigBind(ImGui::Begin, "CMergedMeshRenderNode Inspector", g_show_CMergedMeshRenderNode_inspector, 0);

		ImGui::Text("CMergedMeshRenderNode Count: %llu", g_CMergedMeshRenderNodes.size());
		ImGui::Separator();

		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(g_CMergedMeshRenderNodes.size()));

		if (ImGui::BeginTable("CMergedMeshRenderNodeTable", 1, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
		{
			size_t push_id = 0;
			while (clipper.Step())
			{
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
				{
					ImGui::PushID(push_id);

					auto obj = g_CMergedMeshRenderNodes[i];
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					auto name = obj->GetName();
					if (ImGui::Selectable(strlen(name) ? name : "No Name", g_selected_CMergedMeshRenderNode_detail_inspector == i))
					{
						g_selected_CMergedMeshRenderNode_detail_inspector = i;
					}

					ImGui::PopID();
					push_id++;
				}
			}
			ImGui::EndTable();
		}

		RenderCMergedMeshRenderNodeDetail();

		ImGui::End();
	}

	static entity_xml_info_t* g_selected_entity_xml_info = nullptr;

	void RenderEntityInspector()
	{
		if (!g_show_entity_inspector->get_value())
		{
			return;
		}

		ImGui::ConfigBind(ImGui::Begin, "Entity List Inspector", g_show_entity_inspector, 0);

		ImGui::Text("Entity Count: %llu", g_entities.size());

		ImGui::Hotkey("Target Entity on Crosshair", g_target_entity_on_crosshair);

		RenderEntityInspectorTable();

		ImGui::End();
	}

	void RenderEntityMetadata(const std::string& id)
	{
		auto& current = g_entity_xml_metadata[id];

		ImGui::Separator();
		ImGui::Text(id.c_str());
		ImGui::Separator();

		if (!current.m_attributes.empty() && ImGui::CollapsingHeader("Attributes"))
		{
			size_t i = 0;
			for (const auto& [key, values] : current.m_attributes)
			{
				ImGui::PushID(i++);
				auto& entity_classes = current.m_attributes_entityclass[key];
				if (ImGui::TreeNode(key.c_str(), "%s (%zu)", key.c_str(), values.size()))
				{
					if (ImGui::TreeNode("Related EntityClass List: %s"))
					{
						for (const auto& entity_class : entity_classes)
						{
							ImGui::BulletText(entity_class.c_str());
						}
						ImGui::TreePop();
					}
					ImGui::Text("Example Values:");
					for (const auto& value : values)
					{
						ImGui::BulletText("%s", value.c_str());
					}
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
		}

		if (!current.m_childrens.empty() && ImGui::CollapsingHeader("Children"))
		{
			for (const auto& child_id : current.m_childrens)
			{
				if (ImGui::TreeNode(child_id.c_str()))
				{
					RenderEntityMetadata(child_id);
					ImGui::TreePop();
				}
			}
		}
	}

	void RenderEntityMetadatas()
	{
		if (!g_show_entity_metadata_inspector->get_value())
		{
			return;
		}

		std::scoped_lock l(g_xml_info_mutex);

		ImGui::ConfigBind(ImGui::Begin, "Entity Metadata Inspector", g_show_entity_metadata_inspector, 0);

		ImGui::Text("Metadata Count: %llu", g_entity_xml_metadata.size());
		ImGui::Separator();

		const std::string root_id = "Entity";
		if (ImGui::TreeNode(root_id.c_str(), "Root: %s", root_id.c_str()))
		{
			RenderEntityMetadata(root_id);
			ImGui::TreePop();
		}

		ImGui::End();
	}

	void RenderEntityXmlInfo(entity_xml_info_t* current, const std::string& entity_class_name)
	{
		// Fetch metadata for editing features
		xml_node_metadata_t& linked_metadata = g_entity_xml_metadata[current->m_id];

		if (!current->m_attributes.empty() || !linked_metadata.m_attributes.empty())
		{
			if (ImGui::CollapsingHeader("Attributes", ImGuiTreeNodeFlags_DefaultOpen))
			{
				for (auto& [key, value] : current->m_attributes)
				{
					ImGui::Text("%s:", key.c_str());

					ImGui::InputText(("##" + key).c_str(), &value);

					if (linked_metadata.m_attributes.contains(key))
					{
						if (ImGui::BeginCombo(("##Combo" + key).c_str(), value.c_str()))
						{
							for (const auto& option : linked_metadata.m_attributes[key])
							{
								bool is_selected = (value == option);
								if (ImGui::Selectable(option.c_str(), is_selected))
								{
									value = option;
								}
								if (is_selected)
								{
									ImGui::SetItemDefaultFocus();
								}
							}
							ImGui::EndCombo();
						}
					}
				}

				static std::string new_attribute_name;

				ImGui::InputText("New Attribute Name", &new_attribute_name);

				if (ImGui::BeginCombo("##AddAttribute",
				                      new_attribute_name.empty() ? "Select Attribute" : new_attribute_name.c_str()))
				{
					for (const auto& [possible_attr_name, possible_attr_values] : linked_metadata.m_attributes)
					{
						bool is_attr_not_used = true;
						for (auto& [key, value] : current->m_attributes)
						{
							if (key == possible_attr_name)
							{
								is_attr_not_used = false;
								break;
							}
						}

						if (is_attr_not_used && linked_metadata.m_attributes_entityclass[possible_attr_name].contains(entity_class_name))
						{
							bool is_selected = (new_attribute_name == possible_attr_name);
							if (ImGui::Selectable(possible_attr_name.c_str(), is_selected))
							{
								new_attribute_name = possible_attr_name;
							}
							if (is_selected)
							{
								ImGui::SetItemDefaultFocus();
							}
						}
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine();
				if (ImGui::Button("Add Attribute") && !new_attribute_name.empty())
				{
					current->m_attributes.emplace_back(new_attribute_name, "");
					new_attribute_name.clear();
				}
			}
		}

		/*ImGui::Separator();
		ImGui::Text("dump child data, class name: %s", entity_class_name.c_str());
		for (const auto& possible_child : linked_metadata.m_childrens)
		{
			for (const auto& entityclassname : linked_metadata.m_childrens_entityclass[possible_child])
			{
				ImGui::Text(entityclassname.c_str());
			}
		}
		ImGui::Separator();*/

		if (!current->m_childrens.empty() || !linked_metadata.m_childrens.empty())
		{
			if (ImGui::CollapsingHeader("Children", ImGuiTreeNodeFlags_DefaultOpen))
			{
				size_t index = 0;
				for (auto& child_info : current->m_childrens)
				{
					ImGui::PushID(index);
					if (child_info)
					{
						if (ImGui::TreeNodeEx(child_info->m_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
						{
							RenderEntityXmlInfo(child_info, entity_class_name);
							ImGui::TreePop();
						}
					}
					ImGui::PopID();
					index++;
				}

				static std::string new_child_id;
				if (ImGui::BeginCombo("##AddChild", new_child_id.empty() ? "Select Child" : new_child_id.c_str()))
				{
					for (const auto& possible_child_id : linked_metadata.m_childrens)
					{
						if (linked_metadata.m_childrens_entityclass[possible_child_id].contains(entity_class_name))
						{
							bool is_selected = (new_child_id == possible_child_id);
							if (ImGui::Selectable(possible_child_id.c_str(), is_selected))
							{
								new_child_id = possible_child_id;
							}
							if (is_selected)
							{
								ImGui::SetItemDefaultFocus();
							}
						}
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine();
				if (ImGui::Button("Add Child") && !new_child_id.empty())
				{
					auto new_child    = new entity_xml_info_t;
					new_child->m_id   = new_child_id;
					new_child->m_name = g_entity_xml_metadata[new_child_id].m_name;
					current->m_childrens.push_back(new_child);
					new_child_id.clear();
				}
			}
		}
	}

	void RenderEntityXmlInfoWindow(entity_xml_info_t* current)
	{
		if (!current)
		{
			return;
		}

		ImGui::Begin("Entity XML Info", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

		RenderEntityXmlInfo(current, current->m_entity_class_name);

		ImGui::End();
	}

	void RenderEntityXmlInfos()
	{
		if (!g_show_entity_xml_infos_inspector->get_value())
		{
			return;
		}

		std::scoped_lock l(g_xml_info_mutex);

		ImGui::ConfigBind(ImGui::Begin, "Entity XML Infos Inspector", g_show_entity_xml_infos_inspector, 0);

		ImGui::Text("Entity Count: %llu", g_entity_xml_infos.size());
		ImGui::Separator();

		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(g_entity_xml_infos.size()));

		if (ImGui::BeginTable("EntityTable", 1, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
		{
			while (clipper.Step())
			{
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
				{
					auto* entity_xml_info = g_entity_xml_infos[i];
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					if (ImGui::Selectable(entity_xml_info->m_entity_name.c_str(), g_selected_entity_xml_info == entity_xml_info))
					{
						g_selected_entity_xml_info = entity_xml_info;
					}
				}
			}
			ImGui::EndTable();
		}

		RenderEntityXmlInfoWindow(g_selected_entity_xml_info);

		ImGui::End();
	}

	void gui::dx_on_tick()
	{
		std::scoped_lock l(lua_manager_extension::g_manager_mutex);

		if (!g_lua_manager)
		{
			return;
		}

		push_theme_colors();

		g_lua_manager->always_draw_independent_gui();

		// The multiplayer window is used to start the connection, so the mod GUI
		// is normally still consuming input when the server accepts the player.
		// Close the GUI once on that transition to return mouse and keyboard
		// control to the game, unless the local developer console is deliberately
		// open. Closing that console while connected completes the deferred GUI
		// close. Keep g_show_multiplayer unchanged so reopening the GUI with its
		// hotkey shows the active session again.
		static auto previous_multiplayer_state =
		    kcd2o::client_state::disconnected;
		static bool developer_console_was_open{};
		const auto multiplayer_state =
		    kcd2o::kcse::ui_client().status().state;
		const bool developer_console_just_closed =
		    developer_console_was_open && !g_show_developer_console;
		if (m_is_open
		    && multiplayer_state == kcd2o::client_state::connected
		    && ((previous_multiplayer_state
		             != kcd2o::client_state::connected
		         && !g_show_developer_console)
		        || developer_console_just_closed))
		{
			toggle(false);
			LOG(INFO) << "Multiplayer accepted; closed the mod GUI and restored game input.";
		}
		previous_multiplayer_state = multiplayer_state;
		developer_console_was_open = g_show_developer_console;

		if (!m_onboarded->ref<bool>())
		{
			static bool onboarding_open = false;
			if (!onboarding_open)
			{
				toggle(true);
				ImGui::OpenPopup("Welcome to KCD2Online");
				onboarding_open = true;
			}

			const auto window_size = ImVec2{600, 400};
			ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);

			RECT rect;
			ImVec2 window_position = ImVec2(640, 360);
			if (GetWindowRect(g_renderer->m_window_handle, &rect))
			{
				float width     = rect.right - rect.left;
				float height    = rect.bottom - rect.top;
				window_position = ImVec2{(width - window_size.x) / 2, (height - window_size.y) / 2};
			}
			ImGui::SetNextWindowPos(window_position, ImGuiCond_Always);

			if (ImGui::BeginPopupModal("Welcome to KCD2Online"))
			{
				//ImGui::SeparatorText("Change the GUI opening key if you wish");
				if (ImGui::Hotkey("Open GUI Keybind", g_gui_toggle))
				{
					editing_gui_keybind = true;
				}

				ImGui::TextWrapped("This window is here so that you can change the keyboard shortcut to open the mod's "
				                   "graphical interface.");
				ImGui::TextWrapped("Once you are done please press the Close button so that you can move or interact "
				                   "with the game window again.");

				if (ImGui::Button("Close"))
				{
					m_onboarded->ref<bool>() = true;
					save_pref();
					ImGui::CloseCurrentPopup();
				}

				ImGui::EndPopup();
			}
		}

		const auto foreground_window = GetForegroundWindow();

		if (g_noclip_enabled && g_noclip_enabled->get_value() && g_player_entity && foreground_window == g_renderer->m_window_handle)
		{
			const auto camera_pos_and_dir = get_camera_position_and_direction();
			auto player_pos               = g_player_entity->GetWorldPos();

			static float previous_z = player_pos.z;

			// Note: This can be better implemented by a Entity.IsGrounded check but only god knows where that is located in the game binary.
			if (player_pos.z < previous_z) // If the Z position has naturally decreased
			{
				player_pos.z += 0.001f; // Add some Z to counteract gravity
			}

			const auto forward = camera_pos_and_dir.second;
			const auto up      = Vec3(0, 0, 1);
			const auto right   = forward.Cross(up).Normalized();

			float speed = g_noclip_speed_default->get_value();

			if (GetAsyncKeyState(g_noclip_speed_multiplier_keybind.get_vk_value()) & 0x80'00)
			{
				speed *= g_noclip_speed_multiplier->get_value();
			}

			if (GetAsyncKeyState(g_noclip_forward.get_vk_value()) & 0x80'00)
			{
				player_pos += forward * speed;
			}
			if (GetAsyncKeyState(g_noclip_backward.get_vk_value()) & 0x80'00)
			{
				player_pos -= forward * speed;
			}
			if (GetAsyncKeyState(g_noclip_left.get_vk_value()) & 0x80'00)
			{
				player_pos -= right * speed;
			}
			if (GetAsyncKeyState(g_noclip_right.get_vk_value()) & 0x80'00)
			{
				player_pos += right * speed;
			}
			if (GetAsyncKeyState(g_noclip_up.get_vk_value()) & 0x80'00)
			{
				player_pos += up * speed;
			}
			if (GetAsyncKeyState(g_noclip_down.get_vk_value()) & 0x80'00)
			{
				player_pos -= up * speed;
			}

			previous_z = player_pos.z;

			g_player_entity->SetWorldPos(player_pos);
		}

		if (g_show_cbrush_inspector->get_value() && g_selected_cbrush_detail_inspector > 0
		    && g_selected_cbrush_detail_inspector < g_cbrushes.size())
		{
			auto& brush = g_cbrushes[g_selected_cbrush_detail_inspector];

			AABB bounds;
			brush->FillBBox(bounds);

			float brush_matrix[3 * 4];
			memcpy(brush_matrix, brush->m_Matrix, sizeof(brush_matrix));

			ImGuiIO& io           = ImGui::GetIO();
			ImDrawList* draw_list = ImGui::GetForegroundDrawList();
			ImVec2 display_size   = io.DisplaySize;

			ImGuizmo::SetOrthographic(false);
			ImGuizmo::SetDrawlist(draw_list);

			auto [view_matrix, projection_matrix] = get_camera_view_and_projection_matrix();

			ImGuizmo::SetRect(0, 0, display_size.x, display_size.y);

			float brush_matrix_4x4[16] = {brush_matrix[0], brush_matrix[1], brush_matrix[2], 0.0f, brush_matrix[4], brush_matrix[5], brush_matrix[6], 0.0f, brush_matrix[8], brush_matrix[9], brush_matrix[10], 0.0f, brush_matrix[3], brush_matrix[7], brush_matrix[11], 1.0f};

			if (ImGuizmo::Manipulate(view_matrix, projection_matrix, ImGuizmo::TRANSLATE | ImGuizmo::ROTATE | ImGuizmo::SCALE, (ImGuizmo::MODE)g_imguizmo_editor_local_or_world, brush_matrix_4x4))
			{
				// Extract updated values back to your 3×4 matrix
				brush_matrix[0]  = brush_matrix_4x4[0];
				brush_matrix[1]  = brush_matrix_4x4[1];
				brush_matrix[2]  = brush_matrix_4x4[2];
				brush_matrix[4]  = brush_matrix_4x4[4];
				brush_matrix[5]  = brush_matrix_4x4[5];
				brush_matrix[6]  = brush_matrix_4x4[6];
				brush_matrix[8]  = brush_matrix_4x4[8];
				brush_matrix[9]  = brush_matrix_4x4[9];
				brush_matrix[10] = brush_matrix_4x4[10];

				brush_matrix[3]  = brush_matrix_4x4[12];
				brush_matrix[7]  = brush_matrix_4x4[13];
				brush_matrix[11] = brush_matrix_4x4[14];

				brush->SetMatrix(brush_matrix);

				//static auto

				// TODO:
				// - Saving brush modifications to a file, uniquely identify a Brush by its name, and its position.
				// - Creating a new brush:
				// Loading and spawning a brush from disk: https://github.com/ValtoGameEngines/CryEngine/blob/d9d2c9f000836f0676e65a90bed40dcc3b1451eb/Code/CryEngine/Cry3DEngine/ObjectsTree_Serialize.cpp#L796
				// Cloning existing brush:  https://github.com/ValtoGameEngines/CryEngine/blob/d9d2c9f000836f0676e65a90bed40dcc3b1451eb/Code/CryEngine/Cry3DEngine/terran_edit.cpp#L651
				// - Adding completly custom assets: CMatMan::LoadMaterial -> CObjMan::LoadStatObjAsync
				// https://github.com/ValtoGameEngines/CryEngine/blob/d9d2c9f000836f0676e65a90bed40dcc3b1451eb/Code/CryEngine/Cry3DEngine/3dEngineLoad.cpp#L822
			}

			// Project and draw bounding box as before
			/*float screen_corners[8][3];
			const Vec3 bbox_corners[8] = {{bounds.min.x, bounds.min.y, bounds.min.z},
			                              {bounds.max.x, bounds.min.y, bounds.min.z},
			                              {bounds.min.x, bounds.max.y, bounds.min.z},
			                              {bounds.max.x, bounds.max.y, bounds.min.z},
			                              {bounds.min.x, bounds.min.y, bounds.max.z},
			                              {bounds.max.x, bounds.min.y, bounds.max.z},
			                              {bounds.min.x, bounds.max.y, bounds.max.z},
			                              {bounds.max.x, bounds.max.y, bounds.max.z}};

			for (int i = 0; i < 8; i++)
			{
				g_CD3D9Renderer_ProjectToScreen(g_CD3D9Renderer, bbox_corners[i].x, bbox_corners[i].y, bbox_corners[i].z, &screen_corners[i][0], &screen_corners[i][1], &screen_corners[i][2]);
			}

			const int edges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};

			ImU32 color = IM_COL32(255, 0, 0, 255);

			for (const auto& edge : edges)
			{
				int idx1 = edge[0];
				int idx2 = edge[1];

				ImVec2 p1(screen_corners[idx1][0] * display_size.x / 100.0f, screen_corners[idx1][1] * display_size.y / 100.0f);
				ImVec2 p2(screen_corners[idx2][0] * display_size.x / 100.0f, screen_corners[idx2][1] * display_size.y / 100.0f);

				draw_list->AddLine(p1, p2, color, 1.0f);
			}*/
		}

		if (g_show_CMergedMeshRenderNode_inspector->get_value() && g_selected_CMergedMeshRenderNode_detail_inspector > 0
		    && g_selected_CMergedMeshRenderNode_detail_inspector < g_CMergedMeshRenderNodes.size())
		{
			auto& obj = g_CMergedMeshRenderNodes[g_selected_CMergedMeshRenderNode_detail_inspector];

			AABB bounds;
			obj->FillBBox(bounds);

			ImGuiIO& io           = ImGui::GetIO();
			ImDrawList* draw_list = ImGui::GetForegroundDrawList();
			ImVec2 display_size   = io.DisplaySize;

			//auto [view_matrix, projection_matrix] = get_camera_view_and_projection_matrix();

			// Project and draw bounding box as before
			float screen_corners[8][3];
			const Vec3 bbox_corners[8] = {{bounds.min.x, bounds.min.y, bounds.min.z},
			                              {bounds.max.x, bounds.min.y, bounds.min.z},
			                              {bounds.min.x, bounds.max.y, bounds.min.z},
			                              {bounds.max.x, bounds.max.y, bounds.min.z},
			                              {bounds.min.x, bounds.min.y, bounds.max.z},
			                              {bounds.max.x, bounds.min.y, bounds.max.z},
			                              {bounds.min.x, bounds.max.y, bounds.max.z},
			                              {bounds.max.x, bounds.max.y, bounds.max.z}};

			for (int i = 0; i < 8; i++)
			{
				g_CD3D9Renderer_ProjectToScreen(g_CD3D9Renderer, bbox_corners[i].x, bbox_corners[i].y, bbox_corners[i].z, &screen_corners[i][0], &screen_corners[i][1], &screen_corners[i][2]);
			}

			const int edges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};

			ImU32 color = IM_COL32(255, 0, 0, 255);

			for (const auto& edge : edges)
			{
				int idx1 = edge[0];
				int idx2 = edge[1];

				ImVec2 p1(screen_corners[idx1][0] * display_size.x / 100.0f, screen_corners[idx1][1] * display_size.y / 100.0f);
				ImVec2 p2(screen_corners[idx2][0] * display_size.x / 100.0f, screen_corners[idx2][1] * display_size.y / 100.0f);

				draw_list->AddLine(p1, p2, color, 1.0f);
			}
		}

		if (m_is_open)
		{
			if (ImGui::BeginMainMenuBar())
			{
				ImGui::SetNextWindowSize({400.0f, 0});
				if (ImGui::BeginMenu("GUI"))
				{
					if (ImGui::Hotkey("Open GUI Keybind", g_gui_toggle))
					{
						editing_gui_keybind = true;
					}

					if (ImGui::Checkbox("Open GUI At Startup", &m_is_open_at_startup->ref<bool>()))
					{
						save_pref();
					}

					ImGui::Checkbox("Let Game Input Go Through GUI Layer", &let_game_input_go_through_gui_layer);
					ImGui::MenuItem(
					    "Developer Console",
					    nullptr,
					    &g_show_developer_console);

					ImGui::EndMenu();
				}

				if (ImGui::BeginMenu("Log"))
				{
					ImGui::ConfigBind(ImGui::Checkbox, "Output to the KCD2Online log\nthe vanilla game log (kcd.log)", g_hook_log_write_enabled);

					ImGui::EndMenu();
				}

				if (ImGui::BeginMenu("Multiplayer"))
				{
					ImGui::MenuItem("Open Multiplayer", nullptr, &g_show_multiplayer);
					ImGui::EndMenu();
				}

				if (ImGui::BeginMenu("Trainer"))
				{
					if (ImGui::BeginMenu("Noclip"))
					{
						ImGui::ConfigBind(ImGui::Checkbox, "Enabled", g_noclip_enabled);

						ImGui::Hotkey("Enabled Keybind", g_noclip_enabled_keybind);

						ImGui::Separator();

						ImGui::Hotkey("Forward", g_noclip_forward);
						ImGui::Hotkey("Backward", g_noclip_backward);
						ImGui::Hotkey("Left", g_noclip_left);
						ImGui::Hotkey("Right", g_noclip_right);
						ImGui::Hotkey("Up", g_noclip_up);
						ImGui::Hotkey("Down", g_noclip_down);

						ImGui::Separator();

						ImGui::ConfigBind(ImGui::DragDouble, "Default Speed", g_noclip_speed_default, 1.0f, 0.1f, 100.0f);

						ImGui::Hotkey("Speed Multiplier Keybind", g_noclip_speed_multiplier_keybind);

						ImGui::ConfigBind(ImGui::DragDouble, "Speed Multiplier", g_noclip_speed_multiplier, 1.0f, 1.0f, 100.0f);

						ImGui::EndMenu();
					}

					ImGui::EndMenu();
				}

				if (ImGui::BeginMenu("Inspectors"))
				{
					if (ImGui::BeginMenu("Entity Inspector"))
					{
						ImGui::ConfigBind(ImGui::Checkbox, "Entity Inspector", g_show_entity_inspector);

						ImGui::Hotkey("Target Entity on Crosshair", g_target_entity_on_crosshair);

						ImGui::EndMenu();
					}

					ImGui::ConfigBind(ImGui::Checkbox, "Entity Metadata Inspector", g_show_entity_metadata_inspector);

					ImGui::ConfigBind(ImGui::Checkbox, "Entity XML Infos Inspector", g_show_entity_xml_infos_inspector);

					ImGui::ConfigBind(ImGui::Checkbox, "CBrush Inspector", g_show_cbrush_inspector);

					ImGui::ConfigBind(ImGui::Checkbox, "CMergedMesh Inspector", g_show_CMergedMeshRenderNode_inspector);

					ImGui::ConfigBind(ImGui::Checkbox, "PTF Inspector", g_show_ptf_inspector);

					ImGui::EndMenu();
				}

				if (ImGui::BeginMenu("Mods"))
				{
					ImGui::Text("Vanilla System Mods");
					for (const auto& module : g_vanilla_mods)
					{
						if (ImGui::BeginMenu(module.m_name.c_str()))
						{
							if (ImGui::BeginMenu("Mod Info"))
							{
								ImGui::Text("Mod ID: %s", module.m_mod_id.c_str());
								ImGui::Text("Description: %s", module.m_description.c_str());
								ImGui::Text("Author: %s", module.m_author.c_str());
								ImGui::Text("Version: %s", module.m_version.c_str());
								ImGui::Text("Folder Name: %s", module.m_folder_name.c_str());
								ImGui::Text("Created On: %s", module.m_created_on.c_str());

								if (module.m_loaded_paks.size())
								{
									if (ImGui::BeginMenu("Loaded Paks"))
									{
										for (const auto& loaded_pak : module.m_loaded_paks)
										{
											ImGui::Text("%s", loaded_pak.c_str());
										}

										ImGui::EndMenu();
									}
								}

								ImGui::EndMenu();
							}

							ImGui::EndMenu();
						}
					}

					ImGui::Separator();

					ImGui::Text("ROM Mods");
					g_lua_manager->draw_menu_bar_callbacks();

					ImGui::EndMenu();
				}

				if (ImGui::BeginMenu("Windows"))
				{
					for (auto& [mod_guid, windows] : lua::window::is_open)
					{
						if (!g_lua_manager->module_exists(mod_guid))
						{
							continue;
						}

						if (ImGui::BeginMenu(mod_guid.c_str()))
						{
							if (ImGui::Button("Open All"))
							{
								for (auto& [window_name, is_window_open] : windows)
								{
									is_window_open = true;
								}
								lua::window::serialize();
							}
							ImGui::SameLine();
							if (ImGui::Button("Close All"))
							{
								for (auto& [window_name, is_window_open] : windows)
								{
									is_window_open = false;
								}
								lua::window::serialize();
							}

							for (auto& [window_name, is_window_open] : windows)
							{
								if (ImGui::Checkbox(window_name.c_str(), &is_window_open))
								{
									lua::window::serialize();
								}
							}

							ImGui::EndMenu();
						}
					}

					ImGui::EndMenu();
				}

				ImGui::EndMainMenuBar();
			}

			ImGui::SetMouseCursor(g_gui->m_mouse_cursor);

			g_lua_manager->draw_independent_gui();
			RenderDeveloperConsole();
			RenderMultiplayer();

			/*if (ImGui::Button("Crash it"))
			{
				*(int*)0xDE'AD = 1;
			}*/

			if (g_CEntitySystem)
			{
				RenderEntityInspector();

				RenderEntityMetadatas();

				RenderEntityXmlInfos();

				RenderCBrushInspector();

				RenderCMergedMeshRenderNodeInspector();
			}

			if (g_show_ptf_inspector->get_value())
			{
				ImGui::ConfigBind(ImGui::Begin, "Patched Table Files (PTF) Inspector", g_show_ptf_inspector, 0);
				if (ImGui::TreeNode("Modified Lines"))
				{
					size_t forloop_id_1 = 0;
					for (const auto& [table_name, modified_line_map] : g_table_name_to_modified_line_to_info)
					{
						ImGui::PushID(forloop_id_1++);

						ImGui::Text("Table: %s", table_name.c_str());
						ImGui::Separator();

						auto& original_data_maps = g_table_name_to_modified_line_to_original_data[table_name];
						size_t forloop_id_2      = 0;
						for (const auto& [modified_line_name, mod_infos] : modified_line_map)
						{
							ImGui::PushID(forloop_id_2++);

							const auto& original_data = original_data_maps[modified_line_name];

							const auto& first_mod   = mod_infos[0];
							const size_t last_mod_i = mod_infos.size() - 1;
							const auto& last_mod    = mod_infos[last_mod_i];
							const float patched_float_2 = *reinterpret_cast<const float*>(last_mod.m_patched_data.data() + (2 * 4));
							const float orig_float_2 = *reinterpret_cast<const float*>(original_data.data() + (2 * 4));
							const std::string title  = std::format("{}: {} (Original: {}) (Last Patch: {})",
                                                                  modified_line_name,
                                                                  patched_float_2,
                                                                  orig_float_2,
                                                                  last_mod.m_mod_name);
							if (ImGui::TreeNode(title.c_str()))
							{
								ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
								ImGui::Text("Original Data");
								if (ImGui::BeginChild("##HexOriginalData", ImVec2(0, 60), true))
								{
									std::string hex_line;
									for (size_t i = 0; i < first_mod.m_patched_data.size(); i++)
									{
										char byte_hex[4];
										std::snprintf(byte_hex, sizeof(byte_hex), "%02X ", original_data[i]);
										hex_line += byte_hex;
									}
									ImGui::TextWrapped("%s", hex_line.c_str());
								}
								ImGui::EndChild();
								ImGui::PopStyleVar();

								ImGui::NewLine();
								ImGui::Separator();

								size_t forloop_id_3 = 0;
								for (const auto& mod_info : mod_infos)
								{
									ImGui::PushID(forloop_id_3++);

									ImGui::Text("Mod: %s", mod_info.m_mod_name.c_str());

									size_t float_count = mod_info.m_patched_data.size() / 4;
									if (float_count > 2)
									{
										ImGui::Text("Floats:");
										ImGui::SameLine();
										for (size_t i = 2; i < float_count; ++i)
										{
											if (i > 2)
											{
												ImGui::SameLine();
											}
											float as_float = *reinterpret_cast<const float*>(mod_info.m_patched_data.data() + (i * 4));
											ImGui::Text("[%zu]: %.3f", i, as_float);
										}
									}

									ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
									ImGui::Text("Patched Data");
									if (ImGui::BeginChild("##HexPatchedData", ImVec2(0, 60), true))
									{
										std::string hex_line;
										for (size_t i = 0; i < mod_info.m_patched_data.size(); i++)
										{
											char byte_hex[4];
											std::snprintf(byte_hex, sizeof(byte_hex), "%02X ", mod_info.m_patched_data[i]);
											hex_line += byte_hex;
										}
										ImGui::TextWrapped("%s", hex_line.c_str());
									}
									ImGui::EndChild();
									ImGui::PopStyleVar();

									ImGui::NewLine();
									ImGui::Separator();

									ImGui::PopID();
								}
								ImGui::TreePop();
							}

							ImGui::PopID();
						}

						ImGui::PopID();
					}

					ImGui::TreePop();
				}

				if (ImGui::TreeNode("Added Lines"))
				{
					size_t forloop_id_1 = 0;
					for (const auto& [table_name, added_line_map] : g_table_name_to_added_line_to_info)
					{
						ImGui::PushID(forloop_id_1++);

						ImGui::Text("Table: %s", table_name.c_str());
						ImGui::Separator();

						size_t forloop_id_2 = 0;
						for (const auto& [added_line_name, mod_infos] : added_line_map)
						{
							ImGui::PushID(forloop_id_2++);

							const auto& first_mod   = mod_infos[0];
							const size_t last_mod_i = mod_infos.size() - 1;
							const auto& last_mod    = mod_infos[last_mod_i];
							const float patched_float_2 = *reinterpret_cast<const float*>(last_mod.m_patched_data.data() + (2 * 4));
							const std::string title =
							    std::format("{}: {} (By: {})", added_line_name, patched_float_2, last_mod.m_mod_name);
							if (ImGui::TreeNode(title.c_str()))
							{
								size_t forloop_id_3 = 0;
								for (const auto& mod_info : mod_infos)
								{
									ImGui::PushID(forloop_id_3++);
									ImGui::Text("Mod: %s", mod_info.m_mod_name.c_str());

									size_t float_count = mod_info.m_patched_data.size() / 4;
									if (float_count > 2)
									{
										ImGui::Text("Floats:");
										ImGui::SameLine();
										for (size_t i = 2; i < float_count; ++i)
										{
											if (i > 2)
											{
												ImGui::SameLine();
											}
											float as_float = *reinterpret_cast<const float*>(mod_info.m_patched_data.data() + (i * 4));
											ImGui::Text("[%zu]: %.3f", i, as_float);
										}
									}

									ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
									ImGui::Text("Data");
									if (ImGui::BeginChild("##HexData", ImVec2(0, 60), true))
									{
										std::string hex_line;
										for (size_t i = 0; i < mod_info.m_patched_data.size(); i++)
										{
											char byte_hex[4];
											std::snprintf(byte_hex, sizeof(byte_hex), "%02X ", mod_info.m_patched_data[i]);
											hex_line += byte_hex;
										}
										ImGui::TextWrapped("%s", hex_line.c_str());
									}
									ImGui::EndChild();
									ImGui::PopStyleVar();

									ImGui::NewLine();
									ImGui::Separator();

									ImGui::PopID();
								}
								ImGui::TreePop();
							}

							ImGui::PopID();
						}

						ImGui::PopID();
					}

					ImGui::TreePop();
				}
				ImGui::End();
			}
		}

		pop_theme_colors();
	}

	void gui::save_default_style()
	{
		memcpy(&m_default_config, &ImGui::GetStyle(), sizeof(ImGuiStyle));
	}

	void gui::restore_default_style()
	{
		memcpy(&ImGui::GetStyle(), &m_default_config, sizeof(ImGuiStyle));
	}

	void gui::push_theme_colors()
	{
		//auto button_color = ImGui::ColorConvertU32ToFloat4(g_gui->button_color);
		auto button_color = ImGui::ColorConvertU32ToFloat4(2'947'901'213);
		auto button_active_color =
		    ImVec4(button_color.x + 0.33f, button_color.y + 0.33f, button_color.z + 0.33f, button_color.w);
		auto button_hovered_color =
		    ImVec4(button_color.x + 0.15f, button_color.y + 0.15f, button_color.z + 0.15f, button_color.w);
		auto frame_color = ImGui::ColorConvertU32ToFloat4(2'942'518'340);
		auto frame_hovered_color =
		    ImVec4(frame_color.x + 0.14f, frame_color.y + 0.14f, frame_color.z + 0.14f, button_color.w);
		auto frame_active_color =
		    ImVec4(frame_color.x + 0.30f, frame_color.y + 0.30f, frame_color.z + 0.30f, button_color.w);

		//ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(g_gui->background_color));
		//ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(g_gui->text_color));
		ImGui::PushStyleColor(ImGuiCol_Button, button_color);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, button_hovered_color);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, button_active_color);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, frame_color);
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, frame_hovered_color);
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, frame_active_color);
	}

	void gui::pop_theme_colors()
	{
		//ImGui::PopStyleColor(8);
		ImGui::PopStyleColor(6);
	}

	void gui::wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
	{
		(void)native_multiplayer_menu::on_window_message(
		    msg,
		    wparam,
		    lparam);
		const auto player_hub_was_open =
		    ingame_player_hub::blocks_game_input();
		const auto social_was_open =
		    ingame_social_panel::blocks_game_input();
		const auto staff_was_open = ingame_staff_panel::blocks_game_input();
		if (!social_was_open && !staff_was_open)
		{
			ingame_player_hub::on_window_message(
			    msg,
			    static_cast<std::uintptr_t>(wparam));
		}
		if (!player_hub_was_open && !staff_was_open)
		{
			ingame_social_panel::on_window_message(
			    msg,
			    static_cast<std::uintptr_t>(wparam));
		}
		if (!player_hub_was_open && !social_was_open)
		{
			ingame_staff_panel::on_window_message(
			    msg,
			    static_cast<std::uintptr_t>(wparam));
		}
		if (!player_hub_was_open && !social_was_open && !staff_was_open
		    && !ingame_player_hub::blocks_game_input()
		    && !ingame_social_panel::blocks_game_input()
		    && !ingame_staff_panel::blocks_game_input())
		{
			ingame_chat::on_window_message(
			    msg,
			    static_cast<std::uintptr_t>(wparam));
		}

		if (msg == WM_RBUTTONUP)
		{
			target_entity_on_screen_cursor();
		}

		if (!m_is_open)
		{
			if (msg == WM_KEYUP && wparam == g_target_entity_on_crosshair.get_vk_value())
			{
				target_entity_on_crosshair();
				target_entity_on_crosshair_include_cbrush();
			}

			if (msg == WM_KEYUP && wparam == g_noclip_enabled_keybind.get_vk_value())
			{
				g_noclip_enabled->set_value(!g_noclip_enabled->get_value());
			}
		}

		if (msg == WM_KEYUP && wparam == g_gui_toggle.get_vk_value())
		{
			// Persist and restore the cursor position between menu instances.
			static POINT cursor_coords{};
			if (m_is_open)
			{
				GetCursorPos(&cursor_coords);
			}
			else if (cursor_coords.x + cursor_coords.y != 0)
			{
				SetCursorPos(cursor_coords.x, cursor_coords.y);
			}

			toggle(editing_gui_keybind || !m_is_open);
			if (editing_gui_keybind)
			{
				editing_gui_keybind = false;
			}

			LOG(DEBUG) << "Toggled Modding GUI to: " << (m_is_open ? "visible" : "hidden");
		}
	}

	// TODO: Cleanup all this
	using ClipCursor_t           = BOOL (*)(const RECT* lpRect);
	ClipCursor_t orig_ClipCursor = nullptr;

	static BOOL hook_ClipCursor(const RECT* lpRect)
	{
		if (!g_gui
		    || (!g_gui->is_open()
		        && !ingame_player_hub::blocks_game_input()
		        && !ingame_social_panel::blocks_game_input()
		        && !ingame_staff_panel::blocks_game_input()))
		{
			return orig_ClipCursor(lpRect);
		}

		return 1;
	}

	void gui::toggle_mouse()
	{
		auto& io = ImGui::GetIO();
		const auto release_mouse = m_is_open
		    || ingame_player_hub::blocks_game_input()
		    || ingame_social_panel::blocks_game_input()
		    || ingame_staff_panel::blocks_game_input();

		// Install the guard during GUI initialization so an in-game panel can be
		// the first overlay opened in a session.
		static bool first_time = true;
		if (first_time)
		{
			first_time = false;

			if (GetModuleHandleA("user32.dll"))
			{
				orig_ClipCursor = &ClipCursor;

				EachImportFunction(::GetModuleHandleA("WHGame.dll"),
				                   "user32.dll",
				                   [](const char* funcname, void*& func)
				                   {
					                   //if (strcmp(funcname, "SetCursorPos") == 0)
					                   if (strcmp(funcname, "ClipCursor") == 0)
					                   {
						                   ForceWrite<void*>(func, hook_ClipCursor);
					                   }
				                   });
			}
			else
			{
				LOG(ERROR) << "no user32 hook setcus";
			}
		}

		if (release_mouse)
		{
			io.MouseDrawCursor  = true;
			io.ConfigFlags     &= ~ImGuiConfigFlags_NoMouse;
			io.ConfigFlags     &= ~ImGuiConfigFlags_NoMouseCursorChange;
			ClipCursor(nullptr);
		}
		else
		{
			io.MouseDrawCursor  = false;
			io.ConfigFlags     |= ImGuiConfigFlags_NoMouse;
			io.ConfigFlags     |= ImGuiConfigFlags_NoMouseCursorChange;
		}
	}

	void gui::init_pref()
	{
		try
		{
			const auto file_name = rom::g_project_name + '-' + rom::g_project_name + '-' + "GUI.cfg";
			m_file_path          = g_file_manager.get_project_folder("config").get_path() / file_name;
			if (std::filesystem::exists(m_file_path))
			{
				m_table = toml::parse_file(m_file_path.c_str());
			}

			auto init_node = [](toml::table& table, toml::node*& node, const char* node_name, bool default_value)
			{
				const auto entry_doesnt_exist = !table.contains(node_name);
				if (entry_doesnt_exist)
				{
					table.insert_or_assign(node_name, default_value);
				}

				node = table.get(node_name);
				if (node == nullptr)
				{
					LOG(ERROR) << "what";
				}

				if (node == nullptr || node->type() != toml::node_type::boolean)
				{
					LOG(WARNING) << "Invalid serialized data. Clearing " << node_name;

					table.insert_or_assign(node_name, default_value);
					node = table.get(node_name);
					if (node == nullptr)
					{
						LOG(ERROR) << "what2";
					}
				}
			};

			constexpr auto is_open_at_startup_name = "is_open_at_startup";
			init_node(m_table, m_is_open_at_startup, is_open_at_startup_name, false);
			constexpr auto onboarded_name = "onboarded";
			init_node(m_table, m_onboarded, onboarded_name, false);

			save_pref();

			toggle(m_is_open_at_startup->ref<bool>());
		}
		catch (const std::exception& e)
		{
			LOG(INFO) << "Failed init hotkeys: " << e.what();

			toggle(false);
		}
	}

	void gui::save_pref()
	{
		std::ofstream file_stream(m_file_path, std::ios::out | std::ios::trunc);
		if (file_stream.is_open())
		{
			file_stream << m_table;
		}
		else
		{
			LOG(WARNING) << "Failed to save pref.";
		}
	}
} // namespace big
