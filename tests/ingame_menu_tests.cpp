#include "gui/ingame_menu.hpp"

#include <cassert>

int main()
{
	using namespace big::ingame_ui;

	page menu;
	menu.id = 42;
	menu.title = "TEST MENU";
	menu.add_button("test.primary", "PRIMARY ACTION");
	menu.add_button(
	    "test.back",
	    "BACK",
	    false,
	    1,
	    "Return to the previous page");
	menu.selected_button = "test.primary";
	assert(menu.valid());
	assert(menu.find_button("test.primary"));
	assert(menu.find_button("missing") == nullptr);

	auto duplicate = menu;
	duplicate.add_button("test.primary", "DUPLICATE");
	assert(!duplicate.valid());

	auto missing_selection = menu;
	missing_selection.selected_button = "missing";
	assert(!missing_selection.valid());

	auto information = menu;
	information.information = information_panel{
	    "ACCOUNT INFORMATION",
	    "A readable block of information that is independent of buttons.",
	    "SCROLL WITH THE MOUSE WHEEL"};
	assert(information.valid());
	information.information->body.clear();
	assert(!information.valid());

	page empty;
	empty.title = "EMPTY";
	assert(!empty.valid());
	return 0;
}
