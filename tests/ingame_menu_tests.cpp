#include "gui/ingame_menu.hpp"
#include "gui/privacy_policy.hpp"
#include "gui/terms_of_service.hpp"

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

	assert(terms_title == "KCD2ONLINE TERMS OF SERVICE");
	assert(terms_body.contains("Version 1.0"));
	assert(terms_body.contains("Effective date: 12 August 2026"));
	assert(terms_body.contains("https://support.kingdom-online.cc"));
	assert(terms_body.contains("Discord user: f02k_"));
	assert(terms_body.contains("usernames, display names, server names, or other identifiers"));
	assert(terms_body.contains("racist, antisemitic, extremist"));
	assert(terms_body.contains("deliberate spelling variations intended to evade moderation"));
	assert(terms_body.contains("may be reset or removed without prior warning"));
	assert(terms_body.contains("laws of the Federal Republic of Germany"));
	assert(terms_body.contains("revokes previously issued client credentials and sessions"));
	assert(!terms_body.contains("["));
	assert(privacy_title == "KCD2ONLINE PRIVACY POLICY");
	assert(privacy_body.contains("Version 1.0"));
	assert(privacy_body.contains("Effective date: 12 August 2026"));
	assert(privacy_body.contains("https://support.kingdom-online.cc"));
	assert(privacy_body.contains("Discord user: f02k_"));
	assert(privacy_body.contains("Nuremberg, Germany"));
	assert(privacy_body.contains("Hetzner Online GmbH"));
	assert(privacy_body.contains("Cloudflare"));
	assert(privacy_body.contains("Windows MachineGuid"));
	assert(privacy_body.contains("machine-readable JSON export"));
	assert(privacy_body.contains("complete account ID"));
	assert(!privacy_body.contains("[HOSTING"));
	return 0;
}
