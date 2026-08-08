#include "multiplayer/world_catalog.hpp"
#include "property/catalog.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
	bool write_catalog(
	    const std::filesystem::path &game_root,
	    std::string_view level,
	    const std::filesystem::path &output)
	{
		kcd2o::protocol::PropertyCatalog catalog;
		std::string error;
		const auto pak = kcd2o::property::level_pak_path(game_root, level);
		if (!kcd2o::property::scan_level_pak(pak, level, catalog, error))
		{
			std::cerr << error << '\n';
			return false;
		}
		std::filesystem::create_directories(output.parent_path());
		std::ofstream stream(output, std::ios::binary | std::ios::trunc);
		if (!stream || !catalog.SerializeToOstream(&stream))
		{
			std::cerr << "could not write " << output << '\n';
			return false;
		}
		std::size_t resources{};
		std::size_t markers{};
		for (const auto &property : catalog.properties())
		{
			resources += static_cast<std::size_t>(property.resources_size());
			markers += static_cast<std::size_t>(
			    property.has_marker_position()
			    && property.marker_entity_guid() != 0);
		}
		std::cout << "level " << catalog.level_id() << ": "
		          << catalog.properties_size() << " properties, " << resources
		          << " resources, " << markers << " home anchors -> " << output
		          << '\n';
		return true;
	}
}

int main(int argc, char **argv)
{
	if (argc != 4)
	{
		std::cerr
		    << "usage: KCD2OnlinePropertyCatalog <KCD2 game root> "
		       "<level id|name|--all> <output file|directory>\n";
		return 2;
	}
	const std::filesystem::path game_root(argv[1]);
	const std::string level(argv[2]);
	const std::filesystem::path output(argv[3]);
	if (level != "--all")
		return write_catalog(game_root, level, output) ? 0 : 1;

	bool success = true;
	for (const auto &candidate : kcd2o::native_world_levels)
	{
		if (!candidate.production)
			continue;
		const auto file = output
		    / ("property_catalog_" + std::string(candidate.id) + ".pb");
		success = write_catalog(game_root, candidate.id, file) && success;
	}
	return success ? 0 : 1;
}
