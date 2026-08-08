#include "server/world_store.hpp"

#include "property/catalog.hpp"
#include "property/service.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>

namespace kcd2o::server
{
	namespace
	{
		constexpr std::uint32_t manifest_schema = 3;

		std::string toml_escape(std::string_view value)
		{
			std::string result;
			result.reserve(value.size());
			for (const char character : value)
			{
				if (character == '\\' || character == '"')
				{
					result.push_back('\\');
				}
				result.push_back(character);
			}
			return result;
		}

		void atomic_replace(
		    const std::filesystem::path &target,
		    std::span<const std::byte> bytes)
		{
			const auto temporary = target.wstring() + L".tmp";
			{
				std::ofstream output(
				    std::filesystem::path(temporary),
				    std::ios::binary | std::ios::trunc);
				if (!output
				    || !output.write(
				        reinterpret_cast<const char *>(bytes.data()),
				        static_cast<std::streamsize>(bytes.size())))
				{
					throw std::runtime_error(
					    "could not write persistent file: " + target.string());
				}
				output.flush();
				if (!output)
				{
					throw std::runtime_error(
					    "could not flush persistent file: " + target.string());
				}
			}
			if (!MoveFileExW(
			        temporary.c_str(),
			        target.c_str(),
			        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				DeleteFileW(temporary.c_str());
				throw std::runtime_error(
				    "could not atomically replace persistent file: "
				    + target.string());
			}
		}

		void atomic_replace(
		    const std::filesystem::path &target,
		    std::string_view text)
		{
			atomic_replace(
			    target,
			    {reinterpret_cast<const std::byte *>(text.data()), text.size()});
		}

		protocol::TransformState configured_spawn(
		    const initial_spawn_config &source)
		{
			protocol::TransformState result;
			result.mutable_position()->set_x(source.x);
			result.mutable_position()->set_y(source.y);
			result.mutable_position()->set_z(source.z);
			result.mutable_rotation()->set_x(source.qx);
			result.mutable_rotation()->set_y(source.qy);
			result.mutable_rotation()->set_z(source.qz);
			result.mutable_rotation()->set_w(source.qw);
			result.mutable_velocity();
			return result;
		}

		std::uint64_t random_u64()
		{
			std::uint64_t value{};
			if (BCryptGenRandom(
			        nullptr,
			        reinterpret_cast<PUCHAR>(&value),
			        sizeof(value),
			        BCRYPT_USE_SYSTEM_PREFERRED_RNG)
			    < 0)
			{
				throw std::runtime_error("BCryptGenRandom failed");
			}
			// TOML integer values are signed 64-bit. Keep the generated seed in
			// that portable range while preserving 63 bits of entropy.
			value &= 0x7FFFFFFFFFFFFFFFULL;
			return value == 0 ? 1 : value;
		}

		bool valid_property_catalog(
		    const protocol::PropertyCatalog &catalog,
		    std::string_view level_id)
		{
			if (catalog.schema() != property::catalog_schema
			    || catalog.level_id() != level_id
			    || catalog.content_fingerprint().empty())
				return false;
			std::unordered_set<std::string> property_ids;
			std::unordered_set<std::uint64_t> resources;
			for (const auto &definition : catalog.properties())
			{
				const bool has_marker_position =
				    definition.has_marker_position();
				const bool has_marker_entity =
				    definition.marker_entity_guid() != 0;
				if (definition.property_id().empty()
				    || definition.level_id() != level_id
				    || definition.anchor_guid().empty()
				    || definition.inferred_name().empty()
				    || definition.source_path().empty()
				    || !std::isfinite(definition.discovery_confidence())
				    || definition.discovery_confidence() < 0.0F
				    || definition.discovery_confidence() > 1.0F
				    || has_marker_position != has_marker_entity
				    || (has_marker_position
				        && (!std::isfinite(definition.marker_position().x())
				            || !std::isfinite(definition.marker_position().y())
				            || !std::isfinite(definition.marker_position().z())))
				    || !property_ids.insert(definition.property_id()).second)
					return false;
				for (const auto &resource : definition.resources())
				{
					if (resource.entity_guid() == 0
					    || resource.kind()
					        == protocol::PROPERTY_RESOURCE_KIND_UNSPECIFIED
					    || !protocol::PropertyResourceKind_IsValid(
					        static_cast<int>(resource.kind()))
					    || !resources.insert(resource.entity_guid()).second)
						return false;
				}
			}
			for (const auto &definition : catalog.properties())
			{
				if (!definition.parent_property_id().empty()
				    && !property_ids.contains(definition.parent_property_id()))
					return false;
			}
			return true;
		}

		bool valid_property_ledger(
		    const protocol::PropertyLedger &ledger,
		    const protocol::PropertyCatalog &catalog)
		{
			if (ledger.schema() != property::ledger_schema
			    || ledger.revision() == 0)
				return false;
			std::unordered_set<std::string> properties;
			for (const auto &definition : catalog.properties())
				properties.insert(definition.property_id());
			std::unordered_set<std::string> assignments;
			for (const auto &assignment : ledger.assignments())
			{
				if (!is_uuid(assignment.assignment_id())
				    || !properties.contains(assignment.property_id())
				    || !is_uuid(assignment.subject_player_id())
				    || (!assignment.granted_by_player_id().empty()
				        && !is_uuid(assignment.granted_by_player_id()))
				    || (!assignment.granted_by_assignment_id().empty()
				        && !is_uuid(assignment.granted_by_assignment_id()))
				    || assignment.role() == protocol::PROPERTY_ROLE_UNSPECIFIED
				    || !protocol::PropertyRole_IsValid(
				        static_cast<int>(assignment.role()))
				    || assignment.created_at_ms() == 0
				    || (assignment.expires_at_ms() != 0
				        && assignment.expires_at_ms()
				            <= assignment.created_at_ms())
				    || !assignments.insert(assignment.assignment_id()).second)
					return false;
			}
			for (const auto &assignment : ledger.assignments())
			{
				const bool owner =
				    assignment.role() == protocol::PROPERTY_ROLE_OWNER;
				if (owner != assignment.granted_by_player_id().empty()
				    || owner != assignment.granted_by_assignment_id().empty())
					return false;
				if (!owner
				    && !assignments.contains(
				        assignment.granted_by_assignment_id()))
					return false;
			}
			return true;
		}
	}

	std::string random_hex(std::size_t byte_count)
	{
		if (byte_count == 0 || byte_count > 1024)
		{
			throw std::invalid_argument("random byte count is invalid");
		}
		std::vector<std::uint8_t> bytes(byte_count);
		if (BCryptGenRandom(
		        nullptr,
		        bytes.data(),
		        static_cast<ULONG>(bytes.size()),
		        BCRYPT_USE_SYSTEM_PREFERRED_RNG)
		    < 0)
		{
			throw std::runtime_error("BCryptGenRandom failed");
		}
		std::ostringstream stream;
		stream << std::hex << std::setfill('0');
		for (const auto byte : bytes)
		{
			stream << std::setw(2) << static_cast<unsigned int>(byte);
		}
		return stream.str();
	}

	std::string random_uuid_v4()
	{
		auto compact = random_hex(16);
		compact[12] = '4';
		compact[16] = '8';
		return compact.substr(0, 8) + '-' + compact.substr(8, 4) + '-'
		    + compact.substr(12, 4) + '-' + compact.substr(16, 4) + '-'
		    + compact.substr(20, 12);
	}

	token_hash hash_token(std::string_view token)
	{
		BCRYPT_ALG_HANDLE algorithm{};
		BCRYPT_HASH_HANDLE hash{};
		token_hash result{};
		if (BCryptOpenAlgorithmProvider(
		        &algorithm,
		        BCRYPT_SHA256_ALGORITHM,
		        nullptr,
		        0)
		    < 0)
		{
			throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
		}
		const auto close_algorithm = [&]
		{
			BCryptCloseAlgorithmProvider(algorithm, 0);
		};
		if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
		{
			close_algorithm();
			throw std::runtime_error("BCryptCreateHash failed");
		}
		const auto status = BCryptHashData(
		    hash,
		    reinterpret_cast<PUCHAR>(const_cast<char *>(token.data())),
		    static_cast<ULONG>(token.size()),
		    0);
		const auto finish = status < 0
		    ? status
		    : BCryptFinishHash(
		        hash,
		        reinterpret_cast<PUCHAR>(result.data()),
		        static_cast<ULONG>(result.size()),
		        0);
		BCryptDestroyHash(hash);
		close_algorithm();
		if (finish < 0)
		{
			throw std::runtime_error("BCrypt token hashing failed");
		}
		return result;
	}

	bool secure_equal(
	    std::span<const std::byte> left,
	    std::span<const std::byte> right)
	{
		if (left.size() != right.size())
		{
			return false;
		}
		unsigned char difference{};
		for (std::size_t index = 0; index < left.size(); ++index)
		{
			difference |= std::to_integer<unsigned char>(
			    left[index] ^ right[index]);
		}
		return difference == 0;
	}

	world_store::world_store(const server_config &config) :
	    m_root(config.world_directory),
	    m_profiles_directory(m_root / "players")
	{
		std::filesystem::create_directories(m_profiles_directory);
		load_or_create(config);
		load_profiles();
		load_world_objects();
		load_world_items();
		load_property_catalog();
		load_property_ledger();
	}

	const session_manifest &world_store::manifest() const
	{
		return m_manifest;
	}

	std::vector<persisted_profile> world_store::profiles() const
	{
		return m_profiles;
	}

	const std::vector<protocol::WorldObjectState> &
	world_store::world_objects() const
	{
		return m_world_objects;
	}

	const std::vector<protocol::WorldItemState> &
	world_store::world_items() const
	{
		return m_world_items;
	}

	const protocol::PropertyCatalog &world_store::property_catalog() const
	{
		return m_property_catalog;
	}

	const protocol::PropertyLedger &world_store::property_ledger() const
	{
		return m_property_ledger;
	}

	std::optional<persisted_profile> world_store::find_by_token(
	    std::string_view token) const
	{
		const auto candidate = hash_token(token);
		const auto iterator = std::ranges::find_if(
		    m_profiles,
		    [&](const persisted_profile &profile)
		    {
			    return secure_equal(candidate, profile.identity_hash);
		    });
		return iterator == m_profiles.end()
		    ? std::nullopt
		    : std::optional{*iterator};
	}

	std::optional<persisted_profile> world_store::find_by_player_id(
	    player_id id) const
	{
		const auto iterator = std::ranges::find_if(
		    m_profiles,
		    [id](const persisted_profile &profile)
		    {
			    return profile.profile.player_id() == id;
		    });
		return iterator == m_profiles.end()
		    ? std::nullopt
		    : std::optional{*iterator};
	}

	std::optional<persisted_profile> world_store::find_by_persistent_id(
	    std::string_view id) const
	{
		const auto iterator = std::ranges::find_if(
		    m_profiles,
		    [&](const persisted_profile &profile)
		    { return profile.profile.persistent_id() == id; });
		return iterator == m_profiles.end()
		    ? std::nullopt
		    : std::optional{*iterator};
	}

	player_id world_store::allocate_player_id()
	{
		const auto result = m_manifest.next_player_id++;
		write_manifest();
		return result;
	}

	void world_store::set_spawn(protocol::TransformState spawn)
	{
		if (!is_finite_transform(spawn)
		    || !normalize_rotation(spawn.mutable_rotation()))
		{
			throw std::invalid_argument("session spawn is invalid");
		}
		m_manifest.spawn = std::move(spawn);
		m_manifest.spawn_valid = true;
		++m_manifest.revision;
		write_manifest();
	}

	void world_store::save_profile(
	    const token_hash &identity_hash,
	    const protocol::PlayerProfile &profile)
	{
		if (!is_valid_profile(profile) || profile.player_id() == 0
		    || !is_uuid(profile.persistent_id()))
		{
			throw std::invalid_argument("persistent player profile is invalid");
		}
		if (std::ranges::any_of(
		        m_profiles,
		        [&](const persisted_profile &stored)
		        {
			        return stored.profile.player_id() != profile.player_id()
			            && stored.profile.persistent_id() == profile.persistent_id();
		        }))
		{
			throw std::invalid_argument("persistent player identity is duplicated");
		}
		auto iterator = std::ranges::find_if(
		    m_profiles,
		    [&](const persisted_profile &stored)
		    {
			    return stored.profile.player_id() == profile.player_id();
		    });
		if (iterator == m_profiles.end())
		{
			m_profiles.push_back({identity_hash, profile});
			iterator = std::prev(m_profiles.end());
		}
		else
		{
			iterator->identity_hash = identity_hash;
			iterator->profile = profile;
		}
		write_profile(*iterator);
	}

	void world_store::save_world_objects(
	    std::span<const protocol::WorldObjectState> objects)
	{
		protocol::StoredWorldObjects stored;
		m_world_objects.assign(objects.begin(), objects.end());
		for (const auto &object : m_world_objects)
		{
			if (!is_valid_world_object_state(object))
				throw std::invalid_argument("persistent world object is invalid");
			*stored.add_objects() = object;
		}
		std::string bytes;
		if (!stored.SerializeToString(&bytes))
			throw std::runtime_error("could not serialize persistent world objects");
		atomic_replace(
		    m_root / "world_objects.pb",
		    {reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()});
	}

	void world_store::save_world_items(
	    std::span<const protocol::WorldItemState> items)
	{
		protocol::StoredWorldItems stored;
		m_world_items.assign(items.begin(), items.end());
		for (const auto &item : m_world_items)
		{
			if (!is_valid_world_item_state(item))
				throw std::invalid_argument("persistent world item is invalid");
			*stored.add_items() = item;
		}
		std::string bytes;
		if (!stored.SerializeToString(&bytes))
			throw std::runtime_error("could not serialize persistent world items");
		atomic_replace(
		    m_root / "world_items.pb",
		    {reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()});
	}

	void world_store::save_property_catalog(
	    const protocol::PropertyCatalog &catalog)
	{
		if (!valid_property_catalog(catalog, m_manifest.level_id))
			throw std::invalid_argument("persistent property catalog is invalid");
		std::string bytes;
		if (!catalog.SerializeToString(&bytes))
			throw std::runtime_error("could not serialize property catalog");
		atomic_replace(
		    m_root / "property_catalog.pb",
		    {reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()});
		m_property_catalog = catalog;
	}

	void world_store::save_property_ledger(
	    const protocol::PropertyLedger &ledger)
	{
		if (!valid_property_ledger(ledger, m_property_catalog))
			throw std::invalid_argument("persistent property ledger is invalid");
		std::string bytes;
		if (!ledger.SerializeToString(&bytes))
			throw std::runtime_error("could not serialize property ledger");
		atomic_replace(
		    m_root / "property_ledger.pb",
		    {reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()});
		m_property_ledger = ledger;
	}

	void world_store::load_or_create(const server_config &config)
	{
		const auto path = m_root / "session.toml";
		if (!std::filesystem::exists(path))
		{
			m_manifest.server_id = random_hex(16);
			m_manifest.session_id = random_hex(16);
			m_manifest.world_seed = random_u64();
			m_manifest.level_id = config.level_id;
			m_manifest.content_hash = config.required_content_hash;
			if (config.initial_spawn)
			{
				m_manifest.spawn = configured_spawn(*config.initial_spawn);
				m_manifest.spawn_valid = true;
			}
			write_manifest();
			return;
		}

		const auto document = toml::parse_file(path.string());
		const auto *session = document["session"].as_table();
		if (!session
		    || (*session)["schema"].value_or(0U) != manifest_schema)
		{
			throw std::runtime_error(
			    "world/session.toml is not persistence schema 3; "
			    "pre-profile-v4 worlds cannot be migrated safely, so configure "
			    "a new empty world directory");
		}
		m_manifest.server_id = (*session)["server_id"].value_or(std::string{});
		m_manifest.session_id = (*session)["session_id"].value_or(std::string{});
		m_manifest.revision = (*session)["revision"].value_or<std::uint64_t>(1);
		m_manifest.world_seed =
		    (*session)["world_seed"].value_or<std::uint64_t>(0);
		m_manifest.level_id = (*session)["level_id"].value_or(std::string{});
		m_manifest.content_hash =
		    (*session)["content_hash"].value_or(std::string{});
		m_manifest.sandbox_mode =
		    (*session)["sandbox_mode"].value_or(std::string{});
		m_manifest.spawn_valid = (*session)["spawn_valid"].value_or(false);
		m_manifest.next_player_id =
		    (*session)["next_player_id"].value_or<std::uint64_t>(1);
		if (m_manifest.server_id.empty() || m_manifest.session_id.empty()
		    || m_manifest.level_id != config.level_id
		    || m_manifest.content_hash != config.required_content_hash
		    || m_manifest.sandbox_mode != "isolated_multiplayer"
		    || m_manifest.next_player_id == 0)
		{
			throw std::runtime_error(
			    "world/session.toml does not match server.toml");
		}
		if (m_manifest.spawn_valid)
		{
			const auto *spawn = document["spawn"].as_table();
			if (!spawn)
			{
				throw std::runtime_error("session spawn is missing");
			}
			auto *position = m_manifest.spawn.mutable_position();
			auto *rotation = m_manifest.spawn.mutable_rotation();
			m_manifest.spawn.mutable_velocity();
			position->set_x((*spawn)["x"].value_or(
			    std::numeric_limits<float>::quiet_NaN()));
			position->set_y((*spawn)["y"].value_or(
			    std::numeric_limits<float>::quiet_NaN()));
			position->set_z((*spawn)["z"].value_or(
			    std::numeric_limits<float>::quiet_NaN()));
			rotation->set_x((*spawn)["qx"].value_or(
			    std::numeric_limits<float>::quiet_NaN()));
			rotation->set_y((*spawn)["qy"].value_or(
			    std::numeric_limits<float>::quiet_NaN()));
			rotation->set_z((*spawn)["qz"].value_or(
			    std::numeric_limits<float>::quiet_NaN()));
			rotation->set_w((*spawn)["qw"].value_or(
			    std::numeric_limits<float>::quiet_NaN()));
			if (!is_finite_transform(m_manifest.spawn)
			    || !normalize_rotation(m_manifest.spawn.mutable_rotation()))
			{
				throw std::runtime_error("session spawn is invalid");
			}
		}
	}

	void world_store::load_profiles()
	{
		std::unordered_set<std::string> persistent_ids;
		for (const auto &entry :
		     std::filesystem::directory_iterator(m_profiles_directory))
		{
			if (!entry.is_regular_file() || entry.path().extension() != ".pb")
			{
				continue;
			}
			std::ifstream input(entry.path(), std::ios::binary);
			const std::string bytes{
			    std::istreambuf_iterator<char>(input),
			    std::istreambuf_iterator<char>()};
			protocol::StoredPlayerProfile stored;
			if ((!input && !input.eof()) || !stored.ParseFromString(bytes)
			    || stored.identity_token_hash().size() != 32
			    || !stored.has_profile()
			    || stored.profile().player_id() == 0)
			{
				throw std::runtime_error(
				    "invalid persistent profile: " + entry.path().string());
			}
			bool migrated_identity = false;
			if (stored.profile().persistent_id().empty())
			{
				auto identity = random_uuid_v4();
				while (persistent_ids.contains(identity))
					identity = random_uuid_v4();
				stored.mutable_profile()->set_persistent_id(std::move(identity));
				migrated_identity = true;
			}
			if (!is_valid_profile(stored.profile())
			    || !is_uuid(stored.profile().persistent_id())
			    || !persistent_ids.insert(stored.profile().persistent_id()).second)
			{
				throw std::runtime_error(
				    "invalid or duplicate persistent player identity: "
				    + entry.path().string());
			}
			persisted_profile profile;
			std::ranges::copy(
			    stored.identity_token_hash(),
			    reinterpret_cast<char *>(profile.identity_hash.data()));
			profile.profile = std::move(*stored.mutable_profile());
			m_profiles.push_back(std::move(profile));
			if (migrated_identity)
				write_profile(m_profiles.back());
		}
	}

	void world_store::load_world_objects()
	{
		const auto path = m_root / "world_objects.pb";
		if (!std::filesystem::exists(path))
			return;
		std::ifstream input(path, std::ios::binary);
		const std::string bytes{
		    std::istreambuf_iterator<char>(input),
		    std::istreambuf_iterator<char>()};
		protocol::StoredWorldObjects stored;
		if ((!input && !input.eof()) || !stored.ParseFromString(bytes)
		    || stored.objects_size() > static_cast<int>(max_world_objects))
		{
			throw std::runtime_error("invalid persistent world object state");
		}
		for (auto &object : *stored.mutable_objects())
		{
			if (!is_valid_world_object_state(object))
				throw std::runtime_error("invalid persistent world object entry");
			m_world_objects.push_back(std::move(object));
		}
	}

	void world_store::load_world_items()
	{
		const auto path = m_root / "world_items.pb";
		if (!std::filesystem::exists(path))
			return;
		std::ifstream input(path, std::ios::binary);
		const std::string bytes{
		    std::istreambuf_iterator<char>(input),
		    std::istreambuf_iterator<char>()};
		protocol::StoredWorldItems stored;
		if ((!input && !input.eof()) || !stored.ParseFromString(bytes)
		    || stored.items_size() > static_cast<int>(max_world_items))
		{
			throw std::runtime_error("invalid persistent world item state");
		}
		for (auto &item : *stored.mutable_items())
		{
			if (!is_valid_world_item_state(item))
				throw std::runtime_error("invalid persistent world item entry");
			m_world_items.push_back(std::move(item));
		}
	}

	void world_store::load_property_catalog()
	{
		const auto path = m_root / "property_catalog.pb";
		if (!std::filesystem::exists(path))
			return;
		std::ifstream input(path, std::ios::binary);
		const std::string bytes{
		    std::istreambuf_iterator<char>(input),
		    std::istreambuf_iterator<char>()};
		if ((!input && !input.eof()) || !m_property_catalog.ParseFromString(bytes)
		    || !valid_property_catalog(m_property_catalog, m_manifest.level_id))
		{
			throw std::runtime_error("invalid persistent property catalog");
		}
	}

	void world_store::load_property_ledger()
	{
		const auto path = m_root / "property_ledger.pb";
		if (!std::filesystem::exists(path))
		{
			m_property_ledger.set_schema(property::ledger_schema);
			m_property_ledger.set_revision(1);
			return;
		}
		std::ifstream input(path, std::ios::binary);
		const std::string bytes{
		    std::istreambuf_iterator<char>(input),
		    std::istreambuf_iterator<char>()};
		if ((!input && !input.eof()) || !m_property_ledger.ParseFromString(bytes)
		    || !valid_property_ledger(m_property_ledger, m_property_catalog))
		{
			throw std::runtime_error("invalid persistent property ledger");
		}
	}

	void world_store::write_manifest() const
	{
		std::ostringstream output;
		output << "[session]\n"
		       << "schema = " << manifest_schema << '\n'
		       << "server_id = \"" << toml_escape(m_manifest.server_id) << "\"\n"
		       << "session_id = \"" << toml_escape(m_manifest.session_id) << "\"\n"
		       << "revision = " << m_manifest.revision << '\n'
		       << "world_seed = " << m_manifest.world_seed << '\n'
		       << "level_id = \"" << toml_escape(m_manifest.level_id) << "\"\n"
		       << "content_hash = \"" << toml_escape(m_manifest.content_hash)
		       << "\"\n"
		       << "sandbox_mode = \""
		       << toml_escape(m_manifest.sandbox_mode) << "\"\n"
		       << "spawn_valid = " << (m_manifest.spawn_valid ? "true" : "false")
		       << '\n'
		       << "next_player_id = " << m_manifest.next_player_id << '\n';
		if (m_manifest.spawn_valid)
		{
			output << "\n[spawn]\n"
			       << std::setprecision(9)
			       << "x = " << m_manifest.spawn.position().x() << '\n'
			       << "y = " << m_manifest.spawn.position().y() << '\n'
			       << "z = " << m_manifest.spawn.position().z() << '\n'
			       << "qx = " << m_manifest.spawn.rotation().x() << '\n'
			       << "qy = " << m_manifest.spawn.rotation().y() << '\n'
			       << "qz = " << m_manifest.spawn.rotation().z() << '\n'
			       << "qw = " << m_manifest.spawn.rotation().w() << '\n';
		}
		atomic_replace(m_root / "session.toml", output.str());
	}

	void world_store::write_profile(const persisted_profile &profile) const
	{
		protocol::StoredPlayerProfile stored;
		stored.set_identity_token_hash(
		    profile.identity_hash.data(),
		    profile.identity_hash.size());
		*stored.mutable_profile() = profile.profile;
		std::string bytes;
		if (!stored.SerializeToString(&bytes))
		{
			throw std::runtime_error("could not serialize persistent profile");
		}
		atomic_replace(
		    m_profiles_directory
		        / (std::to_string(profile.profile.player_id()) + ".pb"),
		    {reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()});
	}
}
