#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace big::ingame_ui
{
	struct button
	{
		std::string id;
		std::string text;
		std::string tooltip;
		bool disabled{};
		int container{};
	};

	struct information_panel
	{
		std::string title;
		std::string body;
		std::string scroll_hint;

		[[nodiscard]] bool valid() const noexcept
		{
			return !title.empty() && !body.empty();
		}
	};

	struct page
	{
		std::uint8_t id{};
		int width{1500};
		int height{325};
		int visible_rows{8};
		int style{248};
		std::string title;
		std::vector<button> buttons;
		std::optional<information_panel> information;
		std::string selected_button;
		bool finalize{true};

		button &add_button(
		    std::string button_id,
		    std::string text,
		    bool disabled = false,
		    int container = 0,
		    std::string tooltip = {})
		{
			return buttons.emplace_back(
			    std::move(button_id),
			    std::move(text),
			    std::move(tooltip),
			    disabled,
			    container);
		}

		[[nodiscard]] const button *find_button(
		    std::string_view button_id) const noexcept
		{
			const auto found = std::ranges::find(
			    buttons,
			    button_id,
			    &button::id);
			return found == buttons.end() ? nullptr : &*found;
		}

		[[nodiscard]] bool valid() const
		{
			if (title.empty() || buttons.empty())
				return false;
			std::unordered_set<std::string_view> ids;
			for (const auto &value : buttons)
			{
				if (value.id.empty() || !ids.insert(value.id).second)
					return false;
			}
			return (!information || information->valid())
			    && (selected_button.empty()
			        || find_button(selected_button) != nullptr);
		}
	};
}
