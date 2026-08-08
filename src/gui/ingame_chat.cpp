#include "gui/ingame_chat.hpp"

#include "gui/renderer.hpp"
#ifdef KCD2Online_NATIVE_MULTIPLAYER_MENU
	#include "gui/native_ui_localization.hpp"
#endif
#include "kcse/client_api.hpp"
#include "kcse/client_proxy.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <string>
#include <string_view>
#include <vector>
#include <Windows.h>

namespace big::ingame_chat
{
	namespace
	{
		constexpr double message_hold_seconds      = 8.0;
		constexpr double message_fade_seconds      = 3.0;
		constexpr std::size_t closed_message_count = 5;
		constexpr std::size_t open_message_count   = 12;
		constexpr std::size_t maximum_input_bytes  = kcd2o::kcse::text_capacity - 1;

		std::atomic_bool g_open{};
		std::atomic_bool g_can_open{};
		std::atomic_bool g_focus_requested{};
		std::atomic_bool g_cancel_requested{};
		std::atomic_bool g_submit_requested{};
		std::atomic_bool g_enter_down{};

		struct chat_view_state
		{
			std::string input;
			std::uint64_t input_generation{};
			std::size_t history_size{};
			kcd2o::player_id last_sender{};
			std::uint64_t last_server_time_ms{};
			std::string last_text;
			double last_activity_time{};
			bool scroll_to_bottom{};
		};

		chat_view_state &view_state()
		{
			static chat_view_state value;
			return value;
		}

		ImVec4 with_alpha(ImVec4 color, float alpha)
		{
			color.w *= alpha;
			return color;
		}

		void trim_to_input_capacity(std::string &text)
		{
			if (text.size() <= maximum_input_bytes)
			{
				return;
			}

			std::size_t length = maximum_input_bytes;
			while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0U) == 0x80U)
			{
				--length;
			}
			text.resize(length);
		}

		bool history_changed(chat_view_state &state, const std::vector<kcd2o::chat_entry> &history)
		{
			const auto size_changed = state.history_size != history.size();
			const auto last_changed =
			    !history.empty()
			    && (state.last_sender != history.back().sender || state.last_server_time_ms != history.back().server_time_ms
			        || state.last_text != history.back().text);

			state.history_size = history.size();
			if (history.empty())
			{
				state.last_sender         = 0;
				state.last_server_time_ms = 0;
				state.last_text.clear();
			}
			else
			{
				state.last_sender         = history.back().sender;
				state.last_server_time_ms = history.back().server_time_ms;
				state.last_text           = history.back().text;
			}
			return size_changed || last_changed;
		}

		void close_chat(chat_view_state &state, bool clear_input)
		{
			const auto was_open = g_open.exchange(false, std::memory_order_acq_rel);
			g_focus_requested.store(false, std::memory_order_release);
			g_cancel_requested.store(false, std::memory_order_release);
			g_submit_requested.store(false, std::memory_order_release);
			if (clear_input && (was_open || !state.input.empty()))
			{
				state.input.clear();
				++state.input_generation;
			}
		}

		std::string chat_text(std::string_view key)
		{
#ifdef KCD2Online_NATIVE_MULTIPLAYER_MENU
			return ingame_ui::localized(key);
#else
			if (key == "chat.title")
			{
				return "MULTIPLAYER CHAT";
			}
			if (key == "chat.help")
			{
				return "Enter: send   Esc: close";
			}
			return "Write a message...";
#endif
		}

		void draw_message(const kcd2o::chat_entry &entry, kcd2o::player_id local_player, float alpha)
		{
			const auto server     = entry.sender == 0;
			const auto local      = entry.sender == local_player && local_player != 0;
			const auto name_color = server ? ImVec4(0.88F, 0.65F, 0.27F, alpha) :
			                        local  ? ImVec4(0.83F, 0.73F, 0.48F, alpha) :
			                                 ImVec4(0.72F, 0.68F, 0.58F, alpha);
			const auto text_color = ImVec4(0.93F, 0.90F, 0.82F, alpha);

			ImGui::PushStyleColor(ImGuiCol_Text, name_color);
			ImGui::TextUnformatted(entry.display_name.c_str());
			ImGui::PopStyleColor();
			ImGui::SameLine(0.0F, 0.0F);
			ImGui::PushStyleColor(ImGuiCol_Text, text_color);
			ImGui::TextWrapped(": %s", entry.text.c_str());
			ImGui::PopStyleColor();
		}

		float closed_alpha(const chat_view_state &state, double now)
		{
			const auto age = now - state.last_activity_time;
			if (age <= message_hold_seconds)
			{
				return 1.0F;
			}
			return std::clamp(static_cast<float>(1.0 - (age - message_hold_seconds) / message_fade_seconds), 0.0F, 1.0F);
		}
	} // namespace

	void render(bool mod_gui_open)
	{
		auto &client         = kcd2o::kcse::ui_client();
		const auto status    = client.status();
		const auto connected = status.state == kcd2o::client_state::connected;
		g_can_open.store(connected && !mod_gui_open, std::memory_order_release);

		auto &state = view_state();
		if (!connected || mod_gui_open)
		{
			close_chat(state, mod_gui_open || !connected);
			if (!connected)
			{
				state.history_size        = 0;
				state.last_sender         = 0;
				state.last_server_time_ms = 0;
				state.last_text.clear();
				state.last_activity_time = 0.0;
			}
			return;
		}

		if (g_cancel_requested.exchange(false, std::memory_order_acq_rel))
		{
			close_chat(state, true);
		}

		const auto history = client.chat_history();
		const auto now     = ImGui::GetTime();
		if (history_changed(state, history))
		{
			state.last_activity_time = now;
			state.scroll_to_bottom   = true;
		}

		const auto open = g_open.load(std::memory_order_acquire);
		if (!open && history.empty())
		{
			return;
		}

		const auto alpha = open ? 1.0F : closed_alpha(state, now);
		if (alpha <= 0.01F)
		{
			return;
		}

		const auto *viewport        = ImGui::GetMainViewport();
		const auto scale            = std::clamp(viewport->WorkSize.y / 1080.0F, 0.78F, 1.25F);
		const auto width            = 560.0F * scale;
		const auto height           = 286.0F * scale;
		const auto bottom_clearance = 142.0F * scale;
		const auto position = ImVec2(viewport->WorkPos.x + 38.0F * scale, viewport->WorkPos.y + viewport->WorkSize.y - bottom_clearance);

		ImGui::SetNextWindowPos(position, ImGuiCond_Always, {0.0F, 1.0F});
		ImGui::SetNextWindowSize({width, open ? height : 0.0F}, ImGuiCond_Always);

		const auto background    = with_alpha(ImVec4(0.050F, 0.041F, 0.031F, open ? 0.91F : 0.72F), alpha);
		const auto border        = with_alpha(ImVec4(0.53F, 0.40F, 0.22F, open ? 0.76F : 0.45F), alpha);
		const auto frame         = with_alpha(ImVec4(0.095F, 0.077F, 0.056F, 0.96F), alpha);
		const auto frame_hovered = with_alpha(ImVec4(0.13F, 0.105F, 0.074F, 0.98F), alpha);
		const auto selection     = with_alpha(ImVec4(0.52F, 0.37F, 0.17F, 0.55F), alpha);

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {15.0F * scale, 12.0F * scale});
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 1.0F);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0F);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {7.0F * scale, 4.0F * scale});
		ImGui::PushStyleColor(ImGuiCol_WindowBg, background);
		ImGui::PushStyleColor(ImGuiCol_Border, border);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, frame);
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, frame_hovered);
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, frame_hovered);
		ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, selection);

		auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse;
		if (!open)
		{
			flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;
		}

		if (ImGui::Begin("##KCD2OnlineIngameChat", nullptr, flags))
		{
			const auto window_position = ImGui::GetWindowPos();
			const auto accent_color = ImGui::ColorConvertFloat4ToU32(with_alpha(ImVec4(0.66F, 0.47F, 0.22F, 0.88F), alpha));
			ImGui::GetWindowDrawList()->AddRectFilled(window_position, {window_position.x + width, window_position.y + 2.0F * scale}, accent_color);

			if (g_renderer && g_renderer->font_small)
			{
				ImGui::PushFont(g_renderer->font_small);
			}

			if (open)
			{
				const auto title = chat_text("chat.title");
				const auto help  = chat_text("chat.help");
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78F, 0.64F, 0.38F, alpha));
				ImGui::TextUnformatted(title.c_str());
				ImGui::PopStyleColor();
				ImGui::SameLine();
				const auto help_width = ImGui::CalcTextSize(help.c_str()).x;
				ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), width - help_width - 17.0F * scale));
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.54F, 0.51F, 0.44F, alpha));
				ImGui::TextUnformatted(help.c_str());
				ImGui::PopStyleColor();
				ImGui::Separator();
			}

			const auto count = open ? open_message_count : closed_message_count;
			const auto first = history.size() > count ? history.size() - count : 0;
			if (open)
			{
				if (ImGui::BeginChild("##KCD2OnlineChatHistory", {0.0F, 182.0F * scale}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysVerticalScrollbar))
				{
					for (std::size_t index = first; index < history.size(); ++index)
					{
						draw_message(history[index], status.local_player_id, alpha);
					}

					if (state.scroll_to_bottom)
					{
						ImGui::SetScrollHereY(1.0F);
						state.scroll_to_bottom = false;
					}
				}
				ImGui::EndChild();
			}
			else
			{
				for (std::size_t index = first; index < history.size(); ++index)
				{
					draw_message(history[index], status.local_player_id, alpha);
				}
			}

			if (open)
			{
				ImGui::Separator();
				if (g_focus_requested.exchange(false, std::memory_order_acq_rel))
				{
					ImGui::SetKeyboardFocusHere();
				}
				ImGui::SetNextItemWidth(-1.0F);
				const auto input_id   = "##KCD2OnlineChatInput" + std::to_string(state.input_generation);
				const auto input_hint = chat_text("chat.input_hint");
				ImGui::InputTextWithHint(input_id.c_str(), input_hint.c_str(), &state.input);
				trim_to_input_capacity(state.input);

				if (g_submit_requested.exchange(false, std::memory_order_acq_rel))
				{
					if (!state.input.empty() && client.send_chat(state.input))
					{
						state.last_activity_time = now;
						state.input.clear();
						++state.input_generation;
						g_focus_requested.store(true, std::memory_order_release);
					}
					else if (!state.input.empty())
					{
						g_focus_requested.store(true, std::memory_order_release);
					}
				}
			}

			if (g_renderer && g_renderer->font_small)
			{
				ImGui::PopFont();
			}
		}
		ImGui::End();

		ImGui::PopStyleColor(6);
		ImGui::PopStyleVar(6);
	}

	void on_window_message(std::uint32_t message, std::uintptr_t wparam) noexcept
	{
		if (message == WM_KILLFOCUS)
		{
			g_enter_down.store(false, std::memory_order_release);
			if (g_open.load(std::memory_order_acquire))
			{
				g_cancel_requested.store(true, std::memory_order_release);
			}
			return;
		}

		if (message == WM_KEYDOWN && wparam == VK_ESCAPE && g_open.load(std::memory_order_acquire))
		{
			g_cancel_requested.store(true, std::memory_order_release);
			return;
		}

		if (wparam != VK_RETURN)
		{
			return;
		}

		if (message == WM_KEYUP)
		{
			g_enter_down.store(false, std::memory_order_release);
			return;
		}
		if (message != WM_KEYDOWN || g_enter_down.exchange(true, std::memory_order_acq_rel))
		{
			return;
		}

		if (g_open.load(std::memory_order_acquire))
		{
			g_submit_requested.store(true, std::memory_order_release);
		}
		else if (g_can_open.load(std::memory_order_acquire))
		{
			g_open.store(true, std::memory_order_release);
			g_focus_requested.store(true, std::memory_order_release);
		}
	}

	bool blocks_game_input() noexcept
	{
		return g_open.load(std::memory_order_acquire) || g_cancel_requested.load(std::memory_order_acquire)
		       || g_submit_requested.load(std::memory_order_acquire);
	}
} // namespace big::ingame_chat
