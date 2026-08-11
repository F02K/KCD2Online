#include "gui/server_ui.hpp"

#include "gui/ingame_chat.hpp"
#include "kcse/client_proxy.hpp"

#include <Windows.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace big::server_ui
{
	namespace
	{
		using json = nlohmann::json;
		json g_state{{"documents", json::array()}, {"bindings", json::array()},
		    {"toasts", json::array()}};
		std::string g_raw;
		bool g_keybind_editor{};
		std::atomic_bool g_capture{};
		std::atomic_bool g_text_input{};
		std::mutex g_mutex;
		std::string g_rebind_resource;
		std::string g_rebind_action;
		std::unordered_map<std::string, std::uint32_t> g_key_overrides;
		std::unordered_map<std::string, std::array<char, 1024>> g_inputs;
		std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point>
		    g_toast_started;
		std::string g_server_id;
		bool g_bindings_loaded{};

		std::filesystem::path bindings_path()
		{
			std::wstring value(32768, L'\0');
			const auto length = GetEnvironmentVariableW(
			    L"LOCALAPPDATA", value.data(), static_cast<DWORD>(value.size()));
			if (length == 0 || length >= value.size()) return {};
			value.resize(length);
			return std::filesystem::path(value) / "KCD2Online" / "resource-keybinds.json";
		}

		void load_bindings()
		{
			if (g_bindings_loaded) return;
			g_bindings_loaded = true;
			try
			{
				std::ifstream input(bindings_path());
				if (!input) return;
				const auto values = json::parse(input);
				for (const auto &[key, value] : values.items())
					if (value.is_number_unsigned() && value.get<std::uint32_t>() <= 255)
						g_key_overrides[key] = value.get<std::uint32_t>();
			}
			catch (...) {}
		}

		void save_bindings()
		{
			try
			{
				const auto path = bindings_path();
				if (path.empty()) return;
				std::filesystem::create_directories(path.parent_path());
				std::ofstream output(path, std::ios::trunc);
				output << json(g_key_overrides).dump(2) << '\n';
			}
			catch (...) {}
		}

		std::string binding_key(std::string_view resource, std::string_view action)
		{
			return g_server_id + ":" + std::string(resource) + ":" + std::string(action);
		}

		std::uint32_t effective_key(const json &binding)
		{
			const auto key = binding_key(binding.value("resource_id", ""),
			    binding.value("action_id", ""));
			const auto found = g_key_overrides.find(key);
			return found == g_key_overrides.end()
			    ? binding.value("virtual_key", 0U) : found->second;
		}

		void prune_inputs()
		{
			std::unordered_set<std::string> active;
			for (const auto &document : g_state.value("documents", json::array()))
			{
				try
				{
					for (const auto &item :
					     document.value("spec", json::object())
					         .value("widgets", json::array()))
						if (item.value("type", "") == "input")
							active.insert(document.value("resource_id", "") + ":"
							    + document.value("document_id", "") + ":"
							    + item.value("id", ""));
				}
				catch (...) {}
			}
			std::erase_if(g_inputs, [&](const auto &entry)
			{
				return !active.contains(entry.first);
			});
		}

		void refresh()
		{
			load_bindings();
			g_server_id = kcd2o::kcse::ui_client().status().server_id;
			auto raw = kcd2o::kcse::ui_client().resource_ui_json();
			if (raw.empty() || raw == g_raw)
				return;
			try
			{
				auto parsed = json::parse(raw);
				if (parsed.is_object())
				{
					g_state = std::move(parsed);
					g_raw = std::move(raw);
					prune_inputs();
				}
			}
			catch (...) {}
		}

		void emit(const json &document, std::string_view control,
		    std::string_view event, json payload = json::object())
		{
			(void)kcd2o::kcse::ui_client().submit_resource_ui_event(
			    document.value("resource_id", ""),
			    document.value("document_id", ""), std::string(control),
			    std::string(event), payload.dump());
		}

		void widget(const json &document, const json &item)
		{
			const auto type = item.value("type", "text");
			const auto id = item.value("id", "");
			const auto text = item.value("text", item.value("label", ""));
			if (item.value("same_line", false)) ImGui::SameLine();
			if (type == "text")
				ImGui::TextWrapped("%s", text.c_str());
			else if (type == "separator")
				ImGui::SeparatorText(text.c_str());
			else if (type == "spacer")
				ImGui::Dummy({1.0F, item.value("height", 8.0F)});
			else if (type == "button")
			{
				if (ImGui::Button((text + "##" + id).c_str()))
					emit(document, id, "click");
			}
			else if (type == "checkbox")
			{
				bool value = item.value("value", false);
				if (ImGui::Checkbox((text + "##" + id).c_str(), &value))
					emit(document, id, "change", {{"value", value}});
			}
			else if (type == "slider")
			{
				float value = item.value("value", 0.0F);
				if (ImGui::SliderFloat((text + "##" + id).c_str(), &value,
				        item.value("min", 0.0F), item.value("max", 1.0F)))
					emit(document, id, "change", {{"value", value}});
			}
			else if (type == "progress")
				ImGui::ProgressBar(std::clamp(item.value("value", 0.0F), 0.0F, 1.0F),
				    {-1.0F, 0.0F}, text.c_str());
			else if (type == "input")
			{
				const auto key = document.value("resource_id", "") + ":"
				    + document.value("document_id", "") + ":" + id;
				auto [found, inserted] = g_inputs.try_emplace(key);
				if (inserted)
				{
					const auto value = item.value("value", "");
					std::memcpy(found->second.data(), value.data(),
					    std::min(value.size(), found->second.size() - 1));
				}
				if (ImGui::InputText((text + "##" + id).c_str(),
				        found->second.data(), found->second.size(),
				        ImGuiInputTextFlags_EnterReturnsTrue))
					emit(document, id, "submit", {{"value", found->second.data()}});
			}
		}

		void render_editor()
		{
			if (!g_keybind_editor) return;
			ImGui::SetNextWindowSize({430.0F, 300.0F}, ImGuiCond_FirstUseEver);
			if (!ImGui::Begin("Server keybinds (F8)", &g_keybind_editor))
			{
				ImGui::End(); return;
			}
			ImGui::TextWrapped("Keybinds apply only to this client. Click a binding, then press the new key.");
			for (const auto &binding : g_state.value("bindings", json::array()))
			{
				const auto resource = binding.value("resource_id", "");
				const auto action = binding.value("action_id", "");
				ImGui::PushID((resource + ":" + action).c_str());
				ImGui::Text("%s", binding.value("label", action).c_str());
				ImGui::SameLine(260.0F);
				const auto label = g_rebind_resource == resource && g_rebind_action == action
				    ? "Press a key..." : "Key " + std::to_string(effective_key(binding));
				if (ImGui::Button(label.c_str(), {135.0F, 0.0F}))
				{
					g_rebind_resource = resource; g_rebind_action = action;
				}
				ImGui::PopID();
			}
			ImGui::End();
		}

		void render_toasts()
		{
			const auto now = std::chrono::steady_clock::now();
			std::vector<std::uint64_t> current;
			std::size_t visible{};
			for (const auto &toast : g_state.value("toasts", json::array()))
			{
				const auto revision = toast.value("revision", std::uint64_t{});
				current.push_back(revision);
				const auto started = g_toast_started.try_emplace(revision, now).first->second;
				if (now - started > std::chrono::seconds(5)) continue;
				const auto &payload = toast.value("payload", json::object());
				const auto text = payload.value("text", payload.value("message", ""));
				if (text.empty()) continue;
				const auto viewport = ImGui::GetMainViewport();
				ImGui::SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x - 24.0F,
				    viewport->WorkPos.y + viewport->WorkSize.y - 24.0F - visible * 54.0F},
				    ImGuiCond_Always, {1.0F, 1.0F});
				ImGui::SetNextWindowBgAlpha(0.92F);
				const auto title = "##server-toast-" + std::to_string(revision);
				if (ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoDecoration
				        | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs
				        | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings))
					ImGui::TextUnformatted(text.c_str());
				ImGui::End();
				++visible;
			}
			std::erase_if(g_toast_started, [&](const auto &entry)
			{
				return std::ranges::find(current, entry.first) == current.end();
			});
		}
	}

	void render(bool mod_gui_open)
	{
		std::scoped_lock lock(g_mutex);
		try
		{
			refresh();
			g_capture.store(false, std::memory_order_release);
			if (!mod_gui_open)
			{
				for (const auto &document : g_state.value("documents", json::array()))
				{
					const auto &spec = document.value("spec", json::object());
					if (!spec.value("visible", true)) continue;
					const auto position = spec.value("position", std::vector<float>{});
					const auto size = spec.value("size", std::vector<float>{});
					if (position.size() == 2) ImGui::SetNextWindowPos({position[0], position[1]}, ImGuiCond_Appearing);
					if (size.size() == 2) ImGui::SetNextWindowSize({size[0], size[1]}, ImGuiCond_Appearing);
					const auto title = spec.value("title", document.value("document_id", "Server UI"));
					const auto unique_title = title + "###" + document.value("resource_id", "")
					    + ":" + document.value("document_id", "");
					json widgets = json::array();
					try { widgets = spec.value("widgets", json::array()); }
					catch (...) {}
					if (ImGui::Begin(unique_title.c_str(), nullptr,
					        spec.value("movable", true) ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoMove))
						for (const auto &item : widgets)
							try { widget(document, item); }
							catch (...) {}
					g_capture.store(g_capture.load(std::memory_order_relaxed)
					    || ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
					    || ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows),
					    std::memory_order_relaxed);
					ImGui::End();
				}
			}
			render_toasts();
			render_editor();
			g_capture.store(g_capture.load(std::memory_order_relaxed)
			    || g_keybind_editor, std::memory_order_release);
			g_text_input.store(ImGui::GetIO().WantTextInput,
			    std::memory_order_release);
		}
		catch (...)
		{
			g_capture.store(false, std::memory_order_release);
			g_text_input.store(false, std::memory_order_release);
		}
	}

	void on_window_message(std::uint32_t message, std::uintptr_t wparam,
	    std::intptr_t) noexcept
	{
		try
		{
			std::scoped_lock lock(g_mutex);
			if (message == WM_KEYUP && wparam == VK_F8)
			{
				g_keybind_editor = !g_keybind_editor;
				return;
			}
			if (message != WM_KEYUP || wparam == 0 || wparam > 255)
				return;
			if (!g_rebind_resource.empty())
			{
				g_key_overrides[binding_key(g_rebind_resource, g_rebind_action)] =
				    static_cast<std::uint32_t>(wparam);
				save_bindings();
				g_rebind_resource.clear(); g_rebind_action.clear();
				return;
			}
			if (g_keybind_editor)
				return;
			if (ingame_chat::blocks_game_input()
			    || g_text_input.load(std::memory_order_acquire))
				return;
			refresh();
			for (const auto &binding : g_state.value("bindings", json::array()))
				if (effective_key(binding) == wparam)
					(void)kcd2o::kcse::ui_client().submit_resource_ui_event(
					    binding.value("resource_id", ""), "",
					    binding.value("action_id", ""), "key", "{\"pressed\":true}");
		}
		catch (...) {}
	}

	bool blocks_game_input() noexcept
	{
		return g_capture.load(std::memory_order_acquire);
	}
}
