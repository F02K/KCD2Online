#include "gui/native_ingame_menu_api.hpp"

#include <Offsets/vtables/IFlashPlayer.h>
#include <Offsets/vtables/IFlashVariableObject.h>
#include <Offsets/vtables/IUIElement.h>
#include <guimodule/C_UIMenu.h>
#include <guimodule/SUITypes.h>

#include <algorithm>
#include <array>
#include <cstddef>

namespace big::ingame_ui
{
	namespace
	{
		constexpr std::ptrdiff_t element_offset = 0x48;
		constexpr std::ptrdiff_t menu_interface_offset = 0x58;
		constexpr std::ptrdiff_t menu_state_offset = 0xA0;
		constexpr std::ptrdiff_t menu_page_offset = 0xA1;
		constexpr auto information_name = "KCD2OnlineInformation";
		constexpr float information_x = 105.0f;
		constexpr float information_y = 245.0f;
		constexpr float body_y = 105.0f;
		constexpr float body_height = 455.0f;
		constexpr float scrollbar_height = body_height;
		constexpr float scrollbar_thumb_height = 72.0f;

		Offsets::IUIElement *element_from(void *menu) noexcept
		{
			return menu
			    ? *reinterpret_cast<Offsets::IUIElement **>(
			          static_cast<std::byte *>(menu) + element_offset)
			    : nullptr;
		}

		bool number_value(const SFlashVarValue &value, double &result) noexcept
		{
			switch (value.GetType())
			{
			case SFlashVarValue::eInt: result = value.GetInt(); return true;
			case SFlashVarValue::eUInt: result = value.GetUInt(); return true;
			case SFlashVarValue::eDouble: result = value.GetDouble(); return true;
			case SFlashVarValue::eFloat: result = value.GetFloat(); return true;
			default: return false;
			}
		}

		bool invoke(
		    Offsets::IFlashVariableObject &object,
		    const char *method,
		    const SFlashVarValue *arguments = nullptr,
		    unsigned int count = 0)
		{
			return object.Invoke(method, arguments, count, nullptr);
		}

		bool draw_rectangle(
		    Offsets::IFlashVariableObject &owner,
		    const char *name,
		    int depth,
		    float x,
		    float y,
		    float width,
		    float height,
		    unsigned int color,
		    float alpha,
		    Offsets::FlashVarPtr *result = nullptr)
		{
			Offsets::FlashVarPtr clip;
			if (!owner.CreateEmptyMovieClip(clip.put(), name, depth) || !clip)
				return false;

			const std::array begin{
			    SFlashVarValue(color),
			    SFlashVarValue(alpha)};
			const std::array move{
			    SFlashVarValue(x),
			    SFlashVarValue(y)};
			const std::array line1{
			    SFlashVarValue(x + width),
			    SFlashVarValue(y)};
			const std::array line2{
			    SFlashVarValue(x + width),
			    SFlashVarValue(y + height)};
			const std::array line3{
			    SFlashVarValue(x),
			    SFlashVarValue(y + height)};
			bool ok = invoke(*clip.get(), "beginFill", begin.data(), begin.size());
			ok = invoke(*clip.get(), "moveTo", move.data(), move.size()) && ok;
			ok = invoke(*clip.get(), "lineTo", line1.data(), line1.size()) && ok;
			ok = invoke(*clip.get(), "lineTo", line2.data(), line2.size()) && ok;
			ok = invoke(*clip.get(), "lineTo", line3.data(), line3.size()) && ok;
			ok = invoke(*clip.get(), "lineTo", move.data(), move.size()) && ok;
			ok = invoke(*clip.get(), "endFill") && ok;
			if (result)
				*result = std::move(clip);
			return ok;
		}

		bool create_text_field(
		    Offsets::IFlashPlayer &player,
		    Offsets::IFlashVariableObject &owner,
		    const char *name,
		    int depth,
		    float x,
		    float y,
		    float width,
		    float height,
		    const std::string &text,
		    float size,
		    unsigned int color,
		    bool bold,
		    bool multiline,
		    Offsets::FlashVarPtr &field)
		{
			const std::array arguments{
			    SFlashVarValue(name),
			    SFlashVarValue(depth),
			    SFlashVarValue(x),
			    SFlashVarValue(y),
			    SFlashVarValue(width),
			    SFlashVarValue(height)};
			if (!invoke(owner, "createTextField", arguments.data(), arguments.size())
			    || !owner.GetMember(name, field.put()) || !field)
				return false;

			bool ok = field->SetMember("wordWrap", SFlashVarValue(multiline));
			ok = field->SetMember("multiline", SFlashVarValue(multiline)) && ok;
			ok = field->SetMember("selectable", SFlashVarValue(false)) && ok;
			ok = field->SetMember("embedFonts", SFlashVarValue(true)) && ok;
			ok = field->SetMember("antiAliasType", SFlashVarValue("advanced")) && ok;
			ok = field->SetMember("mouseWheelEnabled", SFlashVarValue(true)) && ok;

			Offsets::FlashVarPtr format;
			if (!player.CreateObject("TextFormat", nullptr, 0, format.put()) || !format)
				return false;
			ok = format->SetMember("font", SFlashVarValue("MicroLatin")) && ok;
			ok = format->SetMember("size", SFlashVarValue(size)) && ok;
			ok = format->SetMember("color", SFlashVarValue(color)) && ok;
			ok = format->SetMember("bold", SFlashVarValue(bold)) && ok;
			ok = format->SetMember("leading", SFlashVarValue(6.0f)) && ok;
			const Offsets::IFlashVariableObject *format_arguments[]{format.get()};
			ok = field->Invoke(
			         "setNewTextFormat",
			         format_arguments,
			         1,
			         nullptr)
			    && ok;
			ok = field->SetText(text.c_str()) && ok;
			ok = field->Invoke(
			         "setTextFormat",
			         format_arguments,
			         1,
			         nullptr)
			    && ok;
			return ok;
		}

		void update_scrollbar(Offsets::IFlashVariableObject &panel)
		{
			Offsets::FlashVarPtr body;
			Offsets::FlashVarPtr track;
			Offsets::FlashVarPtr thumb;
			Offsets::FlashVarPtr hint;
			if (!panel.GetMember("body", body.put()) || !body)
				return;

			SFlashVarValue current_value;
			SFlashVarValue maximum_value;
			double current = 1.0;
			double maximum = 1.0;
			(void)body->GetMember("scroll", current_value);
			(void)body->GetMember("maxscroll", maximum_value);
			(void)number_value(current_value, current);
			(void)number_value(maximum_value, maximum);
			const bool scrollable = maximum > 1.0;

			if (panel.GetMember("scrollTrack", track.put()) && track)
				(void)track->SetVisible(scrollable);
			if (panel.GetMember("scrollThumb", thumb.put()) && thumb)
			{
				(void)thumb->SetVisible(scrollable);
				if (scrollable)
				{
					const auto progress = std::clamp(
					    (current - 1.0) / (maximum - 1.0),
					    0.0,
					    1.0);
					(void)thumb->SetMember(
					    "_y",
					    SFlashVarValue(static_cast<float>(
					        body_y + progress
					            * (scrollbar_height - scrollbar_thumb_height))));
				}
			}
			if (panel.GetMember("scrollHint", hint.put()) && hint)
				(void)hint->SetVisible(scrollable);
		}
	}

	native_menu_api::native_menu_api(void *menu) noexcept
	    : m_menu(menu), m_element(element_from(menu))
	{
	}

	bool native_menu_api::available() const noexcept
	{
		return m_menu && m_element;
	}

	Offsets::IUIElement *native_menu_api::element() const noexcept
	{
		return m_element;
	}

	std::uint8_t native_menu_api::current_page() const noexcept
	{
		return m_menu
		    ? *reinterpret_cast<const std::uint8_t *>(
		          static_cast<const std::byte *>(m_menu) + menu_page_offset)
		    : 0;
	}

	std::uint8_t native_menu_api::mode() const noexcept
	{
		return m_menu
		    ? *reinterpret_cast<const std::uint8_t *>(
		          static_cast<const std::byte *>(m_menu) + menu_state_offset)
		    : 0;
	}

	bool native_menu_api::call(
	    const char *function,
	    const SUIArguments &arguments) const
	{
		return m_element
		    && m_element->CallFunction(function, arguments, nullptr, nullptr);
	}

	bool native_menu_api::append_button(const button &value) const
	{
		if (!available() || value.id.empty())
			return false;
		SUIArguments arguments;
		arguments.AddArgument(value.id.c_str());
		arguments.AddArgument(value.container);
		arguments.AddArgument(value.text.c_str());
		arguments.AddArgument(value.tooltip.c_str());
		arguments.AddArgument(value.disabled);
		return call("AddBasicButton", arguments);
	}

	bool native_menu_api::show(const page &value) const
	{
		if (!available() || !value.valid())
			return false;

		*reinterpret_cast<std::uint8_t *>(
		    static_cast<std::byte *>(m_menu) + menu_page_offset) = value.id;
		SUIArguments empty;
		bool rendered = call("ClearAll", empty);
		clear_information();

		SUIArguments color;
		color.AddArgument(static_cast<int>(mode()));
		rendered = call("SetMenuColor", color) && rendered;

		SUIArguments prepare;
		prepare.AddArgument(value.width);
		prepare.AddArgument(value.height);
		prepare.AddArgument(value.visible_rows);
		prepare.AddArgument(value.title.c_str());
		prepare.AddArgument(value.style);
		rendered = call("PreparePage", prepare) && rendered;

		for (const auto &item : value.buttons)
			rendered = append_button(item) && rendered;
		if (value.finalize)
			rendered = call("ShowPage", empty) && rendered;
		if (!value.selected_button.empty())
		{
			SUIArguments select;
			select.AddArgument(value.selected_button.c_str());
			select.AddArgument(0);
			rendered = call("SelectButton", select) && rendered;
		}
		if (value.information)
			rendered = show_information(*value.information) && rendered;
		return rendered;
	}

	bool native_menu_api::show_information(
	    const information_panel &value) const
	{
		if (!m_element || !value.valid())
			return false;
		auto player = m_element->GetFlashPlayer();
		if (!player)
			return false;

		Offsets::FlashVarPtr root;
		if (!player->GetVariable("_root", root.put()) || !root)
			return false;

		Offsets::FlashVarPtr panel;
		if (!root->CreateEmptyMovieClip(
		        panel.put(), information_name, 32000) || !panel)
			return false;
		bool rendered = panel->SetMember("_x", SFlashVarValue(information_x));
		rendered = panel->SetMember("_y", SFlashVarValue(information_y)) && rendered;
		rendered = draw_rectangle(
		    *panel.get(), "background", 1, 0.0f, 0.0f, 1100.0f, 625.0f,
		    0x090807u, 62.0f) && rendered;
		rendered = draw_rectangle(
		    *panel.get(), "accent", 2, 0.0f, 0.0f, 7.0f, 625.0f,
		    0xC4B58Cu, 88.0f) && rendered;

		Offsets::FlashVarPtr title;
		rendered = create_text_field(
		    *player, *panel.get(), "title", 10, 35.0f, 27.0f, 1000.0f, 55.0f,
		    value.title, 31.0f, 0xF6E890u, true, false, title) && rendered;

		Offsets::FlashVarPtr body;
		rendered = create_text_field(
		    *player, *panel.get(), "body", 11, 35.0f, body_y, 980.0f,
		    body_height, value.body, 25.0f, 0xFFFFFFu, false, true, body)
		    && rendered;

		Offsets::FlashVarPtr hint;
		rendered = create_text_field(
		    *player, *panel.get(), "scrollHint", 12, 35.0f, 580.0f, 1000.0f,
		    28.0f, value.scroll_hint, 18.0f, 0x999999u, false, false, hint)
		    && rendered;

		rendered = draw_rectangle(
		    *panel.get(), "scrollTrack", 20, 1062.0f, body_y, 4.0f,
		    scrollbar_height, 0x777777u, 45.0f) && rendered;
		rendered = draw_rectangle(
		    *panel.get(), "scrollThumb", 21, 1057.0f, body_y, 14.0f,
		    scrollbar_thumb_height, 0xF6E890u, 92.0f) && rendered;
		update_scrollbar(*panel.get());
		return rendered;
	}

	bool native_menu_api::scroll_information(int lines) const
	{
		if (!m_element || lines == 0)
			return false;
		auto player = m_element->GetFlashPlayer();
		if (!player)
			return false;
		Offsets::FlashVarPtr panel;
		Offsets::FlashVarPtr body;
		if (!player->GetVariable(
		        "_root.KCD2OnlineInformation", panel.put()) || !panel
		    || !panel->GetMember("body", body.put()) || !body)
			return false;

		SFlashVarValue current_value;
		SFlashVarValue maximum_value;
		double current = 1.0;
		double maximum = 1.0;
		if (!body->GetMember("scroll", current_value)
		    || !body->GetMember("maxscroll", maximum_value)
		    || !number_value(current_value, current)
		    || !number_value(maximum_value, maximum))
			return false;
		const auto target = static_cast<int>(std::clamp(
		    current + static_cast<double>(lines), 1.0, std::max(1.0, maximum)));
		const bool changed = body->SetMember("scroll", SFlashVarValue(target));
		update_scrollbar(*panel.get());
		return changed;
	}

	void native_menu_api::clear_information() const
	{
		if (!m_element)
			return;
		auto player = m_element->GetFlashPlayer();
		if (!player)
			return;
		Offsets::FlashVarPtr panel;
		if (player->GetVariable(
		        "_root.KCD2OnlineInformation", panel.put()) && panel)
			(void)invoke(*panel.get(), "removeMovieClip");
	}

	void native_menu_api::close() const
	{
		if (!m_menu)
			return;
		clear_information();
		auto *interface_pointer =
		    static_cast<std::byte *>(m_menu) + menu_interface_offset;
		auto **vtable = *reinterpret_cast<void ***>(interface_pointer);
		using close_function = void(__fastcall *)(void *);
		reinterpret_cast<close_function>(vtable[2])(interface_pointer);
	}

	bool native_menu_api::open_root() const
	{
		if (!m_menu)
			return false;
		clear_information();

		using wh::guimodule::E_ButtonId;
		E_ButtonId::Type selection{};
		switch (mode())
		{
		case 1:
		case 4:
			selection = E_ButtonId::Continue;
			break;
		case 2:
		case 3:
			selection = E_ButtonId::Resume;
			break;
		default:
			return false;
		}

		reinterpret_cast<wh::guimodule::C_UIMenu *>(m_menu)
		    ->RebuildRootPage(selection);
		return true;
	}
}
