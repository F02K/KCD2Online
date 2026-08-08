#include "npc/catalog.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include <pugixml.hpp>
#include <zip.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace kcd2o::npc
{
	namespace
	{
		struct human_archetype
		{
			std::string name;
			std::string gender;
		};

		bool is_soul_document(std::string_view name)
		{
			constexpr std::string_view prefix = "libs/tables/rpg/soul";
			if (name.size() < prefix.size() + 4)
				return false;
			std::string lowered(name);
			std::ranges::transform(
			    lowered,
			    lowered.begin(),
			    [](unsigned char value)
			    {
				    return static_cast<char>(std::tolower(value));
			    });
			if (!lowered.starts_with(prefix) || !lowered.ends_with(".xml"))
				return false;
			const auto suffix = std::string_view(lowered).substr(prefix.size());
			return suffix == ".xml"
			    || (suffix.starts_with("__") && !suffix.contains('/'));
		}

		bool parse_documents(
		    std::span<const catalog_document> documents,
		    std::vector<archetype> &output,
		    std::string &error)
		{
			std::map<std::string, human_archetype> human_types;
			for (const auto &document : documents)
			{
				if (!std::string_view(document.source).ends_with(
				        "soul_archetype.xml"))
				{
					continue;
				}
				pugi::xml_document xml;
				if (!xml.load_buffer(document.xml.data(), document.xml.size()))
				{
					error = "could not parse " + document.source;
					return false;
				}
				for (const auto node :
				     xml.child("database")
				         .child("soul_archetypes")
				         .children("soul_archetype"))
				{
					if (std::string_view(
					        node.attribute("race_id").value())
					    != "0")
					{
						continue;
					}
					human_types.emplace(
					    node.attribute("soul_archetype_id").value(),
					    human_archetype{
					        node.attribute("soul_archetype_name").value(),
					        node.attribute("gender_id").value()});
				}
			}
			if (human_types.empty())
			{
				error = "soul_archetype.xml contains no human archetypes";
				return false;
			}

			std::unordered_set<std::string> ids;
			output.clear();
			for (const auto &document : documents)
			{
				if (!is_soul_document(document.source))
					continue;
				pugi::xml_document xml;
				if (!xml.load_buffer(document.xml.data(), document.xml.size()))
				{
					error = "could not parse " + document.source;
					return false;
				}
				for (const auto node :
				     xml.child("database").child("souls").children("soul"))
				{
					const auto type = human_types.find(
					    node.attribute("soul_archetype_id").value());
					if (type == human_types.end())
						continue;
					const std::string soul_id =
					    node.attribute("soul_id").value();
					const std::string soul_name =
					    node.attribute("soul_name").value();
					if (soul_id.empty() || soul_name.empty()
					    || !ids.insert(soul_id).second)
					{
						continue;
					}
					output.push_back({
					    soul_id,
					    soul_name,
					    node.attribute("skald_character_name").value(),
					    type->second.name,
					    type->second.gender,
					    document.source});
				}
			}
			std::ranges::sort(
			    output,
			    [](const archetype &left, const archetype &right)
			    {
				    if (left.soul_name != right.soul_name)
					    return left.soul_name < right.soul_name;
				    return left.soul_id < right.soul_id;
			    });
			if (output.empty())
			{
				error = "Tables.pak contains no human soul entries";
				return false;
			}
			if (!ids.contains(std::string(default_soul_id)))
			{
				error = "Tables.pak does not contain the built-in default soul";
				return false;
			}
			return true;
		}

#ifdef _WIN32
		std::optional<std::filesystem::path> installed_tables_pak()
		{
			std::array<char, MAX_PATH> module_path{};
			const auto length = GetModuleFileNameA(
			    nullptr,
			    module_path.data(),
			    static_cast<DWORD>(module_path.size()));
			if (length == 0 || length == module_path.size())
				return std::nullopt;
			auto current = std::filesystem::path(
			    std::string(module_path.data(), length));
			for (auto parent = current.parent_path(); !parent.empty();
			     parent = parent.parent_path())
			{
				const auto candidate = parent / "Data" / "Tables.pak";
				if (std::filesystem::is_regular_file(candidate))
					return candidate;
				if (parent == parent.root_path())
					break;
			}
			return std::nullopt;
		}
#endif
	}

	bool catalog::load_tables_pak(
	    const std::filesystem::path &path,
	    std::string &error)
	{
		const auto encoded = path.string();
		auto *archive = zip_open(encoded.c_str(), 0, 'r');
		if (!archive)
		{
			error = "could not open NPC catalog source: " + encoded;
			return false;
		}
		std::vector<catalog_document> documents;
		const auto count = zip_entries_total(archive);
		for (int index = 0; index < count; ++index)
		{
			if (zip_entry_openbyindex(archive, index) != 0)
				continue;
			const std::string name =
			    zip_entry_name(archive) ? zip_entry_name(archive) : "";
			const bool wanted = is_soul_document(name)
			    || std::string_view(name).ends_with("soul_archetype.xml");
			if (!wanted || zip_entry_isdir(archive))
			{
				zip_entry_close(archive);
				continue;
			}
			const auto size = zip_entry_size(archive);
			std::string xml(static_cast<std::size_t>(size), '\0');
			const auto read = zip_entry_noallocread(
			    archive,
			    xml.data(),
			    xml.size());
			zip_entry_close(archive);
			if (read < 0 || static_cast<std::size_t>(read) != xml.size())
			{
				zip_close(archive);
				error = "could not read " + name + " from Tables.pak";
				return false;
			}
			documents.push_back({name, std::move(xml)});
		}
		zip_close(archive);
		std::ranges::sort(
		    documents,
		    {},
		    &catalog_document::source);
		return load_documents(documents, error);
	}

	bool catalog::load_documents(
	    std::span<const catalog_document> documents,
	    std::string &error)
	{
		std::vector<archetype> parsed;
		if (!parse_documents(documents, parsed, error))
			return false;
		m_entries = std::move(parsed);
		rebuild_index();
		error.clear();
		return true;
	}

	bool catalog::load_json(
	    const std::filesystem::path &path,
	    std::string &error)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input)
		{
			error = "could not open NPC catalog: " + path.string();
			return false;
		}
		try
		{
			const auto document = nlohmann::json::parse(input);
			if (document.value("schema_version", 0) != 1)
			{
				error = "NPC catalog schema version is incompatible";
				return false;
			}
			if (document.value("default_soul_id", std::string{})
			    != default_soul_id)
			{
				error = "NPC catalog default Soul ID is incompatible";
				return false;
			}
			std::vector<archetype> parsed;
			std::unordered_set<std::string> ids;
			for (const auto &entry : document.at("archetypes"))
			{
				archetype value{
				    entry.at("soul_id").get<std::string>(),
				    entry.at("soul_name").get<std::string>(),
				    entry.value("character_id", std::string{}),
				    entry.at("archetype_name").get<std::string>(),
				    entry.value("gender", std::string{}),
				    entry.at("source").get<std::string>()};
				if (value.soul_id.empty() || value.soul_name.empty()
				    || !ids.insert(value.soul_id).second)
				{
					error = "NPC catalog contains an invalid or duplicate Soul";
					return false;
				}
				parsed.push_back(std::move(value));
			}
			if (!ids.contains(std::string(default_soul_id)))
			{
				error = "NPC catalog does not contain the built-in default Soul";
				return false;
			}
			m_entries = std::move(parsed);
			rebuild_index();
			if (fingerprint() != supported_catalog_fingerprint
			    || size() != supported_catalog_size)
			{
				error = "NPC catalog does not match supported retail build 1308617_856";
				m_entries.clear();
				m_index.clear();
				return false;
			}
			error.clear();
			return true;
		}
		catch (const std::exception &exception)
		{
			error = "could not parse NPC catalog: "
			    + std::string(exception.what());
			return false;
		}
	}

	const archetype *catalog::find(std::string_view soul_id) const
	{
		const auto found = m_index.find(std::string(soul_id));
		return found == m_index.end() ? nullptr : &m_entries[found->second];
	}

	bool catalog::contains(std::string_view soul_id) const
	{
		return find(soul_id) != nullptr;
	}

	std::string catalog::normalize(std::string_view soul_id) const
	{
		return contains(soul_id)
		    ? std::string(soul_id)
		    : std::string(default_soul_id);
	}

	const std::vector<archetype> &catalog::entries() const
	{
		return m_entries;
	}

	std::size_t catalog::size() const
	{
		return m_entries.size();
	}

	std::uint64_t catalog::fingerprint() const
	{
		std::uint64_t result = 14695981039346656037ULL;
		for (const auto &entry : m_entries)
		{
			const std::array fields{
			    std::string_view(entry.soul_id),
			    std::string_view(entry.soul_name),
			    std::string_view(entry.character_id),
			    std::string_view(entry.archetype_name),
			    std::string_view(entry.gender),
			    std::string_view(entry.source)};
			for (const auto field : fields)
			{
				for (const auto byte : field)
				{
					result ^= static_cast<unsigned char>(byte);
					result *= 1099511628211ULL;
				}
				result ^= 0;
				result *= 1099511628211ULL;
			}
		}
		return result;
	}

	void catalog::rebuild_index()
	{
		m_index.clear();
		for (std::size_t index = 0; index < m_entries.size(); ++index)
			m_index.emplace(m_entries[index].soul_id, index);
	}

	catalog &runtime_catalog()
	{
		static catalog result;
		return result;
	}

	bool initialize_runtime_catalog(std::string &error)
	{
#ifdef _WIN32
		const auto path = installed_tables_pak();
		if (!path)
		{
			error = "could not locate Data/Tables.pak from the game executable";
			return false;
		}
		if (!runtime_catalog().load_tables_pak(*path, error))
			return false;
		if (runtime_catalog().size() != supported_catalog_size
		    || runtime_catalog().fingerprint()
		        != supported_catalog_fingerprint)
		{
			error =
			    "local Tables.pak NPC catalog does not match supported retail build 1308617_856";
			return false;
		}
		return true;
#else
		error = "runtime NPC catalog loading is only supported by the Windows client";
		return false;
#endif
	}
}
