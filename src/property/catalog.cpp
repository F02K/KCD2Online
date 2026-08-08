#include "property/catalog.hpp"

#include "multiplayer/world_catalog.hpp"

#include <pugixml.hpp>
#include <zip.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kcd2o::property
{
	namespace
	{
		struct link
		{
			std::uint64_t target_guid{};
			std::uint64_t target_id{};
			std::string name;
		};

		struct entity
		{
			std::uint64_t id{};
			std::uint64_t guid{};
			std::string guid_text;
			std::string name;
			std::string entity_class;
			std::string editor_path;
			std::string normalized_path;
			std::string labels;
			std::optional<protocol::Vec3> position;
			std::vector<link> links;
		};

		struct candidate
		{
			std::string root;
			std::set<std::uint64_t> anchors;
			std::set<std::uint64_t> explicit_doors;
			float confidence{};
			bool scheduler_backed{};
			std::uint64_t marker_anchor{};
		};

		std::optional<protocol::Vec3> parse_position(std::string_view value)
		{
			protocol::Vec3 result;
			float components[3]{};
			std::size_t begin{};
			for (auto index = 0U; index < 3; ++index)
			{
				const auto end = index == 2
				    ? value.size()
				    : value.find(',', begin);
				if (end == std::string_view::npos || end == begin)
					return std::nullopt;
				const auto parsed = std::from_chars(
				    value.data() + begin, value.data() + end, components[index]);
				if (parsed.ec != std::errc{}
				    || parsed.ptr != value.data() + end
				    || !std::isfinite(components[index]))
					return std::nullopt;
				begin = end + 1;
			}
			result.set_x(components[0]);
			result.set_y(components[1]);
			result.set_z(components[2]);
			return result;
		}

		std::string lower_ascii(std::string value)
		{
			std::ranges::transform(
			    value,
			    value.begin(),
			    [](unsigned char character)
			    { return static_cast<char>(std::tolower(character)); });
			return value;
		}

		std::optional<std::uint64_t> parse_unsigned(
		    std::string_view value,
		    int base = 10)
		{
			std::uint64_t result{};
			const auto parsed = std::from_chars(
			    value.data(), value.data() + value.size(), result, base);
			if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
				return std::nullopt;
			return result;
		}

		std::optional<std::uint64_t> parse_entity_guid(std::string_view value)
		{
			const auto first = value.find('-');
			const auto second = first == std::string_view::npos
			    ? std::string_view::npos
			    : value.find('-', first + 1);
			if (first != 8 || second != 13 || value.size() != 18)
				return std::nullopt;
			const auto low = parse_unsigned(value.substr(0, 8), 16);
			const auto middle = parse_unsigned(value.substr(9, 4), 16);
			const auto high = parse_unsigned(value.substr(14, 4), 16);
			if (!low || !middle || !high)
				return std::nullopt;
			return *low | (*middle << 32U) | (*high << 48U);
		}

		std::string format_entity_guid(std::uint64_t value)
		{
			return std::format(
			    "{:08x}-{:04x}-{:04x}",
			    static_cast<std::uint32_t>(value),
			    static_cast<std::uint16_t>(value >> 32U),
			    static_cast<std::uint16_t>(value >> 48U));
		}

		std::vector<std::string> split_path(std::string_view value)
		{
			std::vector<std::string> result;
			std::size_t begin{};
			while (begin < value.size())
			{
				const auto end = value.find('/', begin);
				const auto segment = value.substr(
				    begin,
				    end == std::string_view::npos ? value.size() - begin : end - begin);
				if (!segment.empty())
					result.emplace_back(segment);
				if (end == std::string_view::npos)
					break;
				begin = end + 1;
			}
			return result;
		}

		bool technical_segment(std::string_view value)
		{
			return value.starts_with('_') || value == "scheduler"
			    || value == "crime" || value == "area" || value == "areas"
			    || value == "audio" || value == "static"
			    || value == "streamed" || value == "streamedforbattle";
		}

		std::string normalize_editor_path(std::string value)
		{
			std::ranges::replace(value, '\\', '/');
			value = lower_ascii(std::move(value));
			auto segments = split_path(value);
			while (!segments.empty() && technical_segment(segments.back()))
				segments.pop_back();
			std::string result;
			for (const auto &segment : segments)
			{
				if (!result.empty())
					result.push_back('/');
				result += segment;
			}
			return result;
		}

		std::string common_path(std::string_view left, std::string_view right)
		{
			const auto a = split_path(left);
			const auto b = split_path(right);
			const auto count = std::min(a.size(), b.size());
			std::string result;
			for (std::size_t index = 0; index < count && a[index] == b[index]; ++index)
			{
				if (!result.empty())
					result.push_back('/');
				result += a[index];
			}
			return result;
		}

		bool path_contains(std::string_view path, std::string_view root)
		{
			return path == root
			    || (path.size() > root.size() && path.starts_with(root)
			        && path[root.size()] == '/');
		}

		std::uint64_t fnv1a_update(
		    std::uint64_t result,
		    std::string_view value)
		{
			for (const unsigned char byte : value)
			{
				result ^= byte;
				result *= 1099511628211ULL;
			}
			return result;
		}

		std::uint64_t fnv1a(std::string_view value)
		{
			return fnv1a_update(14695981039346656037ULL, value);
		}

		bool read_entry(
		    zip_t *archive,
		    std::string_view wanted,
		    std::string &output,
		    std::string &error)
		{
			const auto count = zip_entries_total(archive);
			for (int index = 0; index < count; ++index)
			{
				if (zip_entry_openbyindex(archive, index) != 0)
					continue;
				const std::string name = zip_entry_name(archive)
				    ? lower_ascii(zip_entry_name(archive))
				    : std::string{};
				if (name != lower_ascii(std::string(wanted)) || zip_entry_isdir(archive))
				{
					zip_entry_close(archive);
					continue;
				}
				const auto raw_size = zip_entry_size(archive);
				if (raw_size <= 0
				    || static_cast<std::uint64_t>(raw_size)
				        > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
				{
					zip_entry_close(archive);
					error = "property catalog XML entry has an invalid size";
					return false;
				}
				output.assign(static_cast<std::size_t>(raw_size), '\0');
				const auto read = zip_entry_noallocread(
				    archive, output.data(), output.size());
				zip_entry_close(archive);
				if (read < 0 || static_cast<std::size_t>(read) != output.size())
				{
					error = "could not read " + std::string(wanted) + " from level.pak";
					return false;
				}
				return true;
			}
			error = "level.pak does not contain " + std::string(wanted);
			return false;
		}

		std::optional<protocol::PropertyResourceKind> classify_resource(
		    const entity &value)
		{
			const auto type = lower_ascii(value.entity_class);
			const auto name = lower_ascii(value.name);
			if (type.contains("door"))
				return protocol::PROPERTY_RESOURCE_KIND_DOOR;
			if (type == "stash" || type.contains("container"))
				return protocol::PROPERTY_RESOURCE_KIND_CONTAINER;
			if (type == "bedtrigger" || name.contains("bedtrigger"))
				return protocol::PROPERTY_RESOURCE_KIND_BED;
			if (type.contains("alchemy") || type.contains("grindstone")
			    || type.contains("workbench") || type.contains("anvil"))
			{
				return protocol::PROPERTY_RESOURCE_KIND_WORKSTATION;
			}
			return std::nullopt;
		}

		std::string inferred_name(std::string_view root)
		{
			const auto separator = root.rfind('/');
			std::string result(root.substr(
			    separator == std::string_view::npos ? 0 : separator + 1));
			std::ranges::replace(result, '_', ' ');
			if (!result.empty())
				result.front() = static_cast<char>(std::toupper(
				    static_cast<unsigned char>(result.front())));
			return result;
		}

		bool overly_broad_root(std::string_view root)
		{
			const auto separator = root.rfind('/');
			const auto leaf = root.substr(
			    separator == std::string_view::npos ? 0 : separator + 1);
			return leaf == "village" || leaf == "city" || leaf == "town";
		}
	}

	bool scan_level_pak(
	    const std::filesystem::path &level_pak,
	    std::string_view level_id,
	    protocol::PropertyCatalog &output,
	    std::string &error)
	{
		const auto encoded = level_pak.string();
		auto *archive = zip_open(encoded.c_str(), 0, 'r');
		if (!archive)
		{
			error = "could not open property catalog source: " + encoded;
			return false;
		}
		std::string objects_xml;
		const bool read = read_entry(
		    archive, "objects_mission0.xml", objects_xml, error);
		if (!read)
		{
			zip_close(archive);
			return false;
		}

		std::vector<entity> entities;
		std::unordered_map<std::uint64_t, std::size_t> by_id;
		std::unordered_map<std::uint64_t, std::size_t> by_guid;
		pugi::xml_document document;
		const auto append_document = [&](
		    std::string_view xml,
		    std::string_view source) -> bool
		{
			document.reset();
			const auto parsed = document.load_buffer(
			    xml.data(), xml.size(), pugi::parse_default);
			if (!parsed)
			{
				error = "could not parse property catalog XML "
				    + std::string(source) + ": " + parsed.description();
				return false;
			}
			std::unordered_map<std::uint64_t, std::uint64_t> local_guids;
			for (const auto node : document.child("Objects").children("Entity"))
			{
				const auto id = parse_unsigned(node.attribute("EntityId").value());
				const auto guid = parse_entity_guid(
				    node.attribute("EntityGuid").value());
				if (id && guid && *id != 0 && *guid != 0)
					local_guids.insert_or_assign(*id, *guid);
			}
			for (const auto node : document.child("Objects").children("Entity"))
			{
				const auto id = parse_unsigned(node.attribute("EntityId").value());
				const auto guid = parse_entity_guid(
				    node.attribute("EntityGuid").value());
				if (!id || !guid || *id == 0 || *guid == 0)
					continue;
				entity value;
				value.id = *id;
				value.guid = *guid;
				value.guid_text = lower_ascii(
				    node.attribute("EntityGuid").value());
				value.name = node.attribute("Name").value();
				value.entity_class = node.attribute("EntityClass").value();
				value.editor_path = node.attribute("EditorLayer").value();
				value.normalized_path = normalize_editor_path(value.editor_path);
				value.position = parse_position(node.attribute("Pos").value());
				if (const auto properties = node.child("Properties"))
					value.labels = lower_ascii(
					    properties.attribute("Label").value());
				for (const auto source_link :
				     node.child("EntityLinks").children("Link"))
				{
					link edge;
					edge.target_id = parse_unsigned(
					    source_link.attribute("TargetId").value()).value_or(0);
					edge.target_guid = parse_entity_guid(
					    source_link.attribute("TargetGuid").value()).value_or(0);
					if (edge.target_guid == 0)
					{
						if (const auto local = local_guids.find(edge.target_id);
						    local != local_guids.end())
							edge.target_guid = local->second;
					}
					edge.name = lower_ascii(
					    source_link.attribute("Name").value());
					value.links.push_back(std::move(edge));
				}
				if (const auto existing = by_guid.find(value.guid);
				    existing != by_guid.end())
				{
					auto &target = entities[existing->second];
					target.links.insert(
					    target.links.end(),
					    std::make_move_iterator(value.links.begin()),
					    std::make_move_iterator(value.links.end()));
					by_id.try_emplace(value.id, existing->second);
					continue;
				}
				by_id.emplace(value.id, entities.size());
				by_guid.emplace(value.guid, entities.size());
				entities.push_back(std::move(value));
			}
			return true;
		};

		std::uint64_t content_fingerprint = fnv1a(objects_xml);
		if (!append_document(objects_xml, "objects_mission0.xml"))
		{
			zip_close(archive);
			return false;
		}
		const auto entry_count = zip_entries_total(archive);
		for (int index = 0; index < entry_count; ++index)
		{
			if (zip_entry_openbyindex(archive, index) != 0)
				continue;
			const std::string name = zip_entry_name(archive)
			    ? lower_ascii(zip_entry_name(archive))
			    : std::string{};
			const bool wanted = name.starts_with("layers/")
			    && name.ends_with(".xml") && !zip_entry_isdir(archive);
			if (!wanted)
			{
				zip_entry_close(archive);
				continue;
			}
			const auto size = zip_entry_size(archive);
			if (size <= 0 || static_cast<std::uint64_t>(size) > 64ULL * 1024 * 1024)
			{
				zip_entry_close(archive);
				continue;
			}
			std::string layer_xml(static_cast<std::size_t>(size), '\0');
			const auto layer_read = zip_entry_noallocread(
			    archive, layer_xml.data(), layer_xml.size());
			zip_entry_close(archive);
			if (layer_read < 0
			    || static_cast<std::size_t>(layer_read) != layer_xml.size())
			{
				zip_close(archive);
				error = "could not read " + name + " from level.pak";
				return false;
			}
			content_fingerprint = fnv1a_update(
			    content_fingerprint, name);
			content_fingerprint = fnv1a_update(
			    content_fingerprint, layer_xml);
			if (!append_document(layer_xml, name))
			{
				zip_close(archive);
				return false;
			}
		}
		zip_close(archive);
		if (entities.empty())
		{
			error = "objects_mission0.xml contains no property entities";
			return false;
		}

		const auto target_of = [&](const link &edge) -> const entity *
		{
			if (edge.target_guid != 0)
			{
				if (const auto found = by_guid.find(edge.target_guid);
				    found != by_guid.end())
					return &entities[found->second];
			}
			if (const auto found = by_id.find(edge.target_id); found != by_id.end())
				return &entities[found->second];
			return nullptr;
		};

		std::map<std::string, candidate> candidates;
		for (const auto &source : entities)
		{
			if (lower_ascii(source.entity_class) != "schedulerhub")
				continue;
			for (const auto &edge : source.links)
			{
				if (!edge.name.starts_with("home_area"))
					continue;
				const auto *area = target_of(edge);
				if (!area || area->normalized_path.empty())
					continue;
				auto root = common_path(source.normalized_path, area->normalized_path);
				if (split_path(root).size() < 3)
					root = area->normalized_path;
				else if (overly_broad_root(root))
					root = !source.normalized_path.empty()
				        && !overly_broad_root(source.normalized_path)
					    ? source.normalized_path
					    : area->normalized_path;
				auto &property = candidates[root];
				property.root = root;
				property.anchors.insert(source.guid);
				property.anchors.insert(area->guid);
				property.confidence = std::max(property.confidence, 0.95F);
				property.scheduler_backed = true;
				property.marker_anchor = source.guid;
				for (const auto &area_edge : area->links)
				{
					if (area_edge.name.starts_with("crime_door"))
					{
						if (const auto *door = target_of(area_edge))
							property.explicit_doors.insert(door->guid);
					}
				}
			}
		}

		for (const auto &area : entities)
		{
			bool crime_area = false;
			for (const auto &edge : area.links)
				crime_area = crime_area || edge.name.starts_with("crime_door");
			if (!crime_area || area.normalized_path.empty())
				continue;
			auto &property = candidates[area.normalized_path];
			property.root = area.normalized_path;
			property.anchors.insert(area.guid);
			property.confidence = std::max(property.confidence, 0.82F);
			if (property.marker_anchor == 0)
				property.marker_anchor = area.guid;
			for (const auto &edge : area.links)
			{
				if (edge.name.starts_with("crime_door"))
				{
					if (const auto *door = target_of(edge))
						property.explicit_doors.insert(door->guid);
				}
			}
		}

		protocol::PropertyCatalog result;
		result.set_schema(catalog_schema);
		result.set_level_id(std::string(canonical_level_id(level_id)));
		result.set_content_fingerprint(std::format(
		    "{:016x}", content_fingerprint));

		std::unordered_map<std::string, int> definitions;
		for (const auto &[root, source] : candidates)
		{
			if (root.empty() || overly_broad_root(root))
				continue;
			const auto definition_index = result.properties_size();
			auto *definition = result.add_properties();
			std::string stable_key = result.level_id();
			stable_key.push_back('\0');
			stable_key += root;
			definition->set_property_id(std::format(
			    "{}:{:016x}", result.level_id(), fnv1a(stable_key)));
			definition->set_level_id(result.level_id());
			definition->set_source_path(root);
			definition->set_inferred_name(inferred_name(root));
			definition->set_discovery_confidence(source.confidence);
			const auto anchor = source.marker_anchor != 0
			    ? source.marker_anchor
			    : (source.anchors.empty() ? 0 : *source.anchors.begin());
			if (anchor != 0)
			{
				definition->set_anchor_guid(format_entity_guid(anchor));
				definition->set_marker_entity_guid(anchor);
				if (const auto found = by_guid.find(anchor);
				    found != by_guid.end() && entities[found->second].position)
				{
					*definition->mutable_marker_position() =
					    *entities[found->second].position;
				}
			}
			definitions.emplace(root, definition_index);
		}

		for (const auto &value : entities)
		{
			auto kind = classify_resource(value);
			const bool forced_door = std::ranges::any_of(
			    candidates,
			    [&](const auto &entry)
			    { return entry.second.explicit_doors.contains(value.guid); });
			if (forced_door)
				kind = protocol::PROPERTY_RESOURCE_KIND_DOOR;
			if (!kind)
				continue;
			int best_index = -1;
			std::size_t best_length{};
			for (const auto &[root, definition_index] : definitions)
			{
				if (!value.normalized_path.empty() && root.size() > best_length
				    && path_contains(value.normalized_path, root))
				{
					best_index = definition_index;
					best_length = root.size();
				}
			}
			if (best_index < 0 && forced_door)
			{
				for (const auto &[root, definition_index] : definitions)
				{
					const auto source = candidates.find(root);
					if (source != candidates.end()
					    && source->second.explicit_doors.contains(value.guid)
					    && root.size() > best_length)
					{
						best_index = definition_index;
						best_length = root.size();
					}
				}
			}
			if (best_index < 0)
				continue;
			auto *best = result.mutable_properties(best_index);
			const auto duplicate = std::ranges::any_of(
			    best->resources(),
			    [&](const protocol::PropertyResource &resource)
			    { return resource.entity_guid() == value.guid; });
			if (!duplicate)
			{
				auto *resource = best->add_resources();
				resource->set_entity_guid(value.guid);
				resource->set_kind(*kind);
			}
		}

		if (result.properties().empty())
		{
			error = "property discovery found no manageable resources";
			return false;
		}
		output = std::move(result);
		error.clear();
		return true;
	}

	std::filesystem::path level_pak_path(
	    const std::filesystem::path &game_root,
	    std::string_view level_id)
	{
		const auto level = find_native_world_level(level_id);
		const auto directory = level ? level->name : level_id;
		return game_root / "Data" / "Levels" / directory / "level.pak";
	}
}
