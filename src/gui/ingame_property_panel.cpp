#include "gui/ingame_property_panel.hpp"

#include "gui/gui.hpp"
#include "gui/renderer.hpp"
#ifdef KCD2Online_NATIVE_MULTIPLAYER_MENU
	#include "gui/native_ui_localization.hpp"
#endif
#include "kcse/client_proxy.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <imgui.h>
#include <ranges>
#include <string>
#include <string_view>
#include <Windows.h>

namespace big::ingame_property_panel
{
	namespace
	{
		std::atomic_bool g_open{};
		std::uint64_t g_entity_guid{};
		std::uint32_t g_action_generation{};
		std::uint32_t g_operation_generation{};
		std::string g_feedback;
		bool g_feedback_success{};
		std::string g_target_player;
		int g_role_index{1};
		int g_expiry_hours{};

		std::string text(std::string_view key)
		{
#ifdef KCD2Online_NATIVE_MULTIPLAYER_MENU
			return ingame_ui::localized(key);
#else
			if (key == "property.title") return "PROPERTY MANAGEMENT";
			if (key == "property.close_hint") return "Esc: close";
			if (key == "property.id") return "PROPERTY ID";
			if (key == "property.own_role") return "YOUR ROLE";
			if (key == "property.tab.roles") return "OWNERSHIP & ROLES";
			if (key == "property.player") return "PLAYER";
			if (key == "property.role") return "ROLE";
			if (key == "property.expiry_hours") return "EXPIRY IN HOURS (0 = PERMANENT)";
			if (key == "property.assign_role") return "ASSIGN ROLE";
			if (key == "property.set_owner") return "SET AS OWNER";
			if (key == "property.remove") return "REMOVE";
			if (key == "property.tab.resources") return "RESOURCES";
			if (key == "property.guid") return "GUID";
			if (key == "property.lock") return "LOCK";
			if (key == "property.unlock") return "UNLOCK";
			if (key == "property.role.owner") return "OWNER";
			if (key == "property.role.steward") return "STEWARD";
			if (key == "property.role.resident") return "RESIDENT";
			if (key == "property.role.employee") return "EMPLOYEE";
			if (key == "property.role.guard") return "GUARD";
			if (key == "property.role.guest") return "GUEST";
			if (key == "property.role.none") return "NONE";
			if (key == "property.resource.door") return "DOOR";
			if (key == "property.resource.container") return "CONTAINER";
			if (key == "property.resource.bed") return "BED";
			if (key == "property.resource.workstation") return "WORKSTATION";
			if (key == "property.resource.generic") return "RESOURCE";
			return std::string(key);
#endif
		}

		void set_open(bool value)
		{
			if (g_open.exchange(value, std::memory_order_acq_rel) != value && g_gui)
				g_gui->sync_mouse_capture();
		}

		std::string role_name(kcd2o::protocol::PropertyRole role)
		{
			switch (role)
			{
			case kcd2o::protocol::PROPERTY_ROLE_OWNER: return text("property.role.owner");
			case kcd2o::protocol::PROPERTY_ROLE_STEWARD: return text("property.role.steward");
			case kcd2o::protocol::PROPERTY_ROLE_RESIDENT: return text("property.role.resident");
			case kcd2o::protocol::PROPERTY_ROLE_EMPLOYEE: return text("property.role.employee");
			case kcd2o::protocol::PROPERTY_ROLE_GUARD: return text("property.role.guard");
			case kcd2o::protocol::PROPERTY_ROLE_GUEST: return text("property.role.guest");
			default: return text("property.role.none");
			}
		}

		std::string resource_kind_name(
		    kcd2o::protocol::PropertyResourceKind kind)
		{
			switch (kind)
			{
			case kcd2o::protocol::PROPERTY_RESOURCE_KIND_DOOR: return text("property.resource.door");
			case kcd2o::protocol::PROPERTY_RESOURCE_KIND_CONTAINER: return text("property.resource.container");
			case kcd2o::protocol::PROPERTY_RESOURCE_KIND_BED: return text("property.resource.bed");
			case kcd2o::protocol::PROPERTY_RESOURCE_KIND_WORKSTATION: return text("property.resource.workstation");
			default: return text("property.resource.generic");
			}
		}

		kcd2o::protocol::PropertyRole selected_role()
		{
			constexpr kcd2o::protocol::PropertyRole roles[] = {
			    kcd2o::protocol::PROPERTY_ROLE_STEWARD,
			    kcd2o::protocol::PROPERTY_ROLE_RESIDENT,
			    kcd2o::protocol::PROPERTY_ROLE_EMPLOYEE,
			    kcd2o::protocol::PROPERTY_ROLE_GUARD,
			    kcd2o::protocol::PROPERTY_ROLE_GUEST};
			return roles[std::clamp(g_role_index, 0, 4)];
		}

		const kcd2o::protocol::PropertyAccess *find_access(
		    const kcd2o::protocol::PropertyAccessSnapshot &snapshot,
		    std::uint64_t entity_guid)
		{
			const auto found = std::ranges::find_if(
			    snapshot.properties(),
			    [&](const auto &access)
			    {
				    return access.can_manage() && std::ranges::any_of(
				        access.property().resources(),
				        [&](const auto &resource)
				        { return resource.entity_guid() == entity_guid; });
			    });
			return found == snapshot.properties().end() ? nullptr : &*found;
		}

		std::string player_name(
		    std::string_view persistent_id,
		    const std::vector<kcd2o::remote_player_view> &players)
		{
			const auto found = std::ranges::find(
			    players, persistent_id, &kcd2o::remote_player_view::persistent_id);
			return found == players.end() ? std::string(persistent_id)
			                              : found->display_name;
		}
	}

	void render(bool another_panel_open)
	{
		auto &client = kcd2o::kcse::ui_client();
		const auto status = client.status();
		const auto snapshot = client.property_access();
		if (status.property_action_generation != g_action_generation)
		{
			g_action_generation = status.property_action_generation;
			g_entity_guid = status.property_action_entity_guid;
			const auto *triggered_access = find_access(snapshot, g_entity_guid);
			set_open(triggered_access != nullptr);
			if (triggered_access)
			{
				LOGF(
				    INFO,
				    "[Property] Management interaction accepted: entity_guid={}, property_id='{}', generation={}, blocked_by_other_panel={}",
				    g_entity_guid,
				    triggered_access->property().property_id(),
				    g_action_generation,
				    another_panel_open);
			}
			else
			{
				LOGF(
				    WARNING,
				    "[Property] Management interaction rejected by UI lookup: entity_guid={}, generation={}, snapshot_properties={}",
				    g_entity_guid,
				    g_action_generation,
				    snapshot.properties_size());
			}
		}
		if (status.property_operation_generation != g_operation_generation)
		{
			g_operation_generation = status.property_operation_generation;
			g_feedback = status.property_operation_message;
			g_feedback_success = status.property_operation_success;
		}
		const auto *access = find_access(snapshot, g_entity_guid);
		if (status.state != kcd2o::client_state::connected || !access)
		{
			set_open(false);
			return;
		}
		if (!g_open.load(std::memory_order_acquire) || another_panel_open)
			return;

		auto players = client.players();
		std::ranges::sort(players, {}, &kcd2o::remote_player_view::display_name);
		if (g_target_player.empty() && !players.empty())
			g_target_player = players.front().persistent_id;

		const auto *viewport = ImGui::GetMainViewport();
		const auto scale =
		    std::clamp(viewport->WorkSize.y / 1080.0F, 0.78F, 1.18F);
		const ImVec2 size{
		    std::min(1080.0F * scale, viewport->WorkSize.x - 36.0F),
		    std::min(700.0F * scale, viewport->WorkSize.y - 36.0F)};
		ImGui::SetNextWindowPos(
		    {viewport->WorkPos.x + viewport->WorkSize.x * 0.5F,
		     viewport->WorkPos.y + viewport->WorkSize.y * 0.5F},
		    ImGuiCond_Always, {0.5F, 0.5F});
		ImGui::SetNextWindowSize(size, ImGuiCond_Always);

		ImGui::PushStyleVar(
		    ImGuiStyleVar_WindowPadding, {16.0F * scale, 14.0F * scale});
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0F);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0F);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
		ImGui::PushStyleVar(
		    ImGuiStyleVar_ItemSpacing, {8.0F * scale, 7.0F * scale});
		ImGui::PushStyleColor(
		    ImGuiCol_WindowBg, {0.050F, 0.041F, 0.031F, 0.97F});
		ImGui::PushStyleColor(
		    ImGuiCol_Border, {0.53F, 0.40F, 0.22F, 0.82F});
		ImGui::PushStyleColor(
		    ImGuiCol_ChildBg, {0.066F, 0.053F, 0.039F, 0.88F});
		ImGui::PushStyleColor(
		    ImGuiCol_FrameBg, {0.095F, 0.077F, 0.056F, 0.96F});
		ImGui::PushStyleColor(
		    ImGuiCol_FrameBgHovered, {0.13F, 0.105F, 0.074F, 0.98F});
		ImGui::PushStyleColor(ImGuiCol_Button, {0.17F, 0.13F, 0.080F, 1.0F});
		ImGui::PushStyleColor(
		    ImGuiCol_ButtonHovered, {0.29F, 0.21F, 0.11F, 1.0F});
		ImGui::PushStyleColor(
		    ImGuiCol_ButtonActive, {0.38F, 0.27F, 0.13F, 1.0F});
		ImGui::PushStyleColor(
		    ImGuiCol_Header, {0.27F, 0.19F, 0.10F, 0.90F});
		ImGui::PushStyleColor(
		    ImGuiCol_HeaderHovered, {0.34F, 0.24F, 0.12F, 0.95F});
		ImGui::PushStyleColor(
		    ImGuiCol_HeaderActive, {0.42F, 0.29F, 0.13F, 1.0F});
		ImGui::PushStyleColor(
		    ImGuiCol_Tab, {0.10F, 0.08F, 0.055F, 1.0F});
		ImGui::PushStyleColor(
		    ImGuiCol_TabHovered, {0.27F, 0.19F, 0.10F, 1.0F});
		ImGui::PushStyleColor(
		    ImGuiCol_TabActive, {0.34F, 0.23F, 0.11F, 1.0F});

		const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
		    | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
		if (ImGui::Begin("##KCD2OnlinePropertyPanel", nullptr, flags))
		{
			const auto position = ImGui::GetWindowPos();
			ImGui::GetWindowDrawList()->AddRectFilled(
			    position,
			    {position.x + size.x, position.y + 3.0F * scale},
			    IM_COL32(183, 126, 49, 235));
			if (g_renderer && g_renderer->font_small)
				ImGui::PushFont(g_renderer->font_small);

			ImGui::PushStyleColor(
			    ImGuiCol_Text, {0.86F, 0.72F, 0.45F, 1.0F});
			const auto title = text("property.title");
			ImGui::TextUnformatted(title.c_str());
			ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::TextDisabled(
			    "  %s", access->property().inferred_name().c_str());
			ImGui::SameLine();
			const auto close_hint = text("property.close_hint");
			const auto close_hint_width = ImGui::CalcTextSize(close_hint.c_str()).x;
			ImGui::SetCursorPosX(std::max(
			    ImGui::GetCursorPosX(),
			    size.x - close_hint_width - 18.0F * scale));
			ImGui::TextDisabled("%s", close_hint.c_str());
			ImGui::Separator();
			ImGui::TextColored(
			    {0.93F, 0.88F, 0.76F, 1.0F},
			    "%s", access->property().inferred_name().c_str());
			const auto property_id_label = text("property.id");
			const auto own_role_label = text("property.own_role");
			const auto own_role = role_name(access->effective_role());
			ImGui::TextDisabled("%s  %s   |   %s  %s",
			    property_id_label.c_str(),
			    access->property().property_id().c_str(),
			    own_role_label.c_str(), own_role.c_str());

			if (!g_feedback.empty())
				ImGui::TextColored(
				    g_feedback_success ? ImVec4{0.35F, 0.85F, 0.45F, 1.0F}
				                       : ImVec4{0.95F, 0.35F, 0.28F, 1.0F},
				    "%s", g_feedback.c_str());
			ImGui::Separator();

			if (ImGui::BeginTabBar("##PropertyTabs"))
			{
				const auto roles_tab = text("property.tab.roles");
				if (ImGui::BeginTabItem(roles_tab.c_str()))
				{
					const auto player_label = text("property.player");
					if (ImGui::BeginCombo(player_label.c_str(), player_name(g_target_player, players).c_str()))
					{
						for (const auto &player : players)
						{
							const bool selected = player.persistent_id == g_target_player;
							if (ImGui::Selectable(player.display_name.c_str(), selected))
								g_target_player = player.persistent_id;
						}
						ImGui::EndCombo();
					}
					const std::array<std::string, 5> role_labels{
					    text("property.role.steward"), text("property.role.resident"),
					    text("property.role.employee"), text("property.role.guard"),
					    text("property.role.guest")};
					const std::array<const char *, 5> role_items{
					    role_labels[0].c_str(), role_labels[1].c_str(),
					    role_labels[2].c_str(), role_labels[3].c_str(),
					    role_labels[4].c_str()};
					const auto role_label = text("property.role");
					ImGui::Combo(role_label.c_str(), &g_role_index, role_items.data(),
					    static_cast<int>(role_items.size()));
					const auto expiry_label = text("property.expiry_hours");
					ImGui::InputInt(expiry_label.c_str(), &g_expiry_hours);
					g_expiry_hours = std::max(g_expiry_hours, 0);
					const auto assign_role_label = text("property.assign_role");
					if (ImGui::Button(assign_role_label.c_str()) && !g_target_player.empty())
					{
						std::uint64_t expiry{};
						if (g_expiry_hours > 0)
						{
							expiry = static_cast<std::uint64_t>(
							    std::chrono::duration_cast<std::chrono::milliseconds>(
							        std::chrono::system_clock::now().time_since_epoch())
							        .count())
							    + static_cast<std::uint64_t>(g_expiry_hours) * 3'600'000ULL;
						}
						(void)client.request_property_role(
						    access->property().property_id(), g_target_player,
						    selected_role(), expiry);
					}
					if (access->can_edit_owner())
					{
						ImGui::SameLine();
						const auto set_owner_label = text("property.set_owner");
						if (ImGui::Button(set_owner_label.c_str()) && !g_target_player.empty())
							(void)client.set_property_owner(
							    access->property().property_id(), g_target_player);
					}
					ImGui::Separator();
					for (const auto &assignment : access->assignments())
					{
						ImGui::PushID(assignment.assignment_id().c_str());
						const auto assignment_role = role_name(assignment.role());
						ImGui::Text("%s — %s",
						    player_name(assignment.subject_player_id(), players).c_str(),
						    assignment_role.c_str());
						if (assignment.role() != kcd2o::protocol::PROPERTY_ROLE_OWNER)
						{
							ImGui::SameLine();
							const auto remove_label = text("property.remove");
							if (ImGui::SmallButton(remove_label.c_str()))
								(void)client.revoke_property_role(assignment.assignment_id());
						}
						ImGui::PopID();
					}
					ImGui::EndTabItem();
				}
				const auto resources_tab = text("property.tab.resources");
				if (ImGui::BeginTabItem(resources_tab.c_str()))
				{
					for (const auto &resource : access->property().resources())
					{
						ImGui::PushID(static_cast<int>(resource.entity_guid()));
						const auto resource_kind = resource_kind_name(resource.kind());
						const auto guid_label = text("property.guid");
						ImGui::Text("%s  |  %s %llu",
						    resource_kind.c_str(), guid_label.c_str(),
						    static_cast<unsigned long long>(resource.entity_guid()));
						if (access->can_secure()
						    && (resource.kind() == kcd2o::protocol::PROPERTY_RESOURCE_KIND_DOOR
						        || resource.kind() == kcd2o::protocol::PROPERTY_RESOURCE_KIND_CONTAINER))
						{
							ImGui::SameLine();
							const auto lock_label = text("property.lock");
							if (ImGui::SmallButton(lock_label.c_str()))
								(void)client.set_property_locked(resource.entity_guid(), true);
							ImGui::SameLine();
							const auto unlock_label = text("property.unlock");
							if (ImGui::SmallButton(unlock_label.c_str()))
								(void)client.set_property_locked(resource.entity_guid(), false);
						}
						ImGui::PopID();
					}
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}

			if (g_renderer && g_renderer->font_small)
				ImGui::PopFont();
		}
		ImGui::End();
		ImGui::PopStyleColor(14);
		ImGui::PopStyleVar(6);
	}

	void on_window_message(std::uint32_t message, std::uintptr_t wparam) noexcept
	{
		if (message == WM_KILLFOCUS
		    || (message == WM_KEYDOWN && wparam == VK_ESCAPE))
			set_open(false);
	}

	bool blocks_game_input() noexcept
	{
		return g_open.load(std::memory_order_acquire);
	}
}
