#include "gui/localization.hpp"

#include <cassert>
#include <filesystem>
#include <string>

int main()
{
	using namespace big::ingame_ui;
	assert(normalize_language("German") == "de");
	assert(normalize_language("de-DE") == "de");
	assert(normalize_language("English") == "en");
	assert(normalize_language("Czech") == "cs");

	const auto directory = std::filesystem::path(KCD2Online_SOURCE_DIR)
	    / "data" / "lang";
	localization_catalog catalog;
	std::string error;
	assert(catalog.load(directory, "german", error));
	assert(error.empty());
	assert(catalog.language() == "de");
	assert(catalog.text("menu.action.back").find("\xC3\x9C")
	    != std::string::npos);
	assert(catalog.text("menu.action.connect.tooltip").find("\xC3\xA4")
	    != std::string::npos);
	assert(catalog.format(
	           "menu.status.connected_server",
	           {{"server", "Kuttenberg"}})
	    == "VERBUNDEN: Kuttenberg");
	assert(catalog.text("chat.title") == "MEHRSPIELER-CHAT");
	assert(catalog.text("chat.help").find("schlie\xC3\x9F" "en")
	    != std::string::npos);
	assert(catalog.format(
	           "browser.details",
	           {{"players", "2"},
	            {"max", "8"},
	            {"level", "3"},
	            {"version", "0.1.3"},
	            {"password", "JA"},
	            {"id", "srv_test"}})
	    == "SPIELER: 2/8\nLEVEL: 3\nVERSION: 0.1.3\nPASSWORT: JA\nSERVER-ID: srv_test");

	assert(catalog.load(directory, "french", error));
	assert(catalog.language() == "fr");
	assert(catalog.text("menu.action.connect") == "CONNECT TO SERVER");
	assert(catalog.text("chat.input_hint") == "Write a message...");
	assert(catalog.text("missing.key") == "[[missing.key]]");
	return 0;
}
