#pragma once

#include "multiplayer/protocol.hpp"
#include "server/server_config.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace kcd2o::server
{
	using token_hash = std::array<std::byte, 32>;

	struct session_manifest
	{
		std::string server_id;
		std::string session_id;
		std::uint64_t revision{1};
		std::uint64_t world_seed{};
		std::string level_id;
		std::string content_hash;
		std::string sandbox_mode{"isolated_multiplayer"};
		bool spawn_valid{};
		protocol::TransformState spawn;
		player_id next_player_id{1};
	};

	struct persisted_profile
	{
		token_hash identity_hash{};
		protocol::PlayerProfile profile;
	};

	[[nodiscard]] std::string random_hex(std::size_t byte_count);
	[[nodiscard]] std::string random_uuid_v4();
	[[nodiscard]] token_hash hash_token(std::string_view token);
	[[nodiscard]] bool secure_equal(
	    std::span<const std::byte> left,
	    std::span<const std::byte> right);

	class world_store
	{
	public:
		explicit world_store(const server_config &config);

		[[nodiscard]] const session_manifest &manifest() const;
		[[nodiscard]] std::vector<persisted_profile> profiles() const;
		[[nodiscard]] const std::vector<protocol::WorldObjectState> &
		world_objects() const;
		[[nodiscard]] const std::vector<protocol::WorldItemState> &
		world_items() const;
		[[nodiscard]] const protocol::PropertyCatalog &property_catalog() const;
		[[nodiscard]] const protocol::PropertyLedger &property_ledger() const;
		[[nodiscard]] std::optional<persisted_profile> find_by_token(
		    std::string_view token) const;
		[[nodiscard]] std::optional<persisted_profile> find_by_player_id(
		    player_id id) const;
		[[nodiscard]] std::optional<persisted_profile> find_by_persistent_id(
		    std::string_view id) const;

		[[nodiscard]] player_id allocate_player_id();
		void set_spawn(protocol::TransformState spawn);
		void save_profile(
		    const token_hash &identity_hash,
		    const protocol::PlayerProfile &profile);
		void save_world_objects(
		    std::span<const protocol::WorldObjectState> objects);
		void save_world_items(
		    std::span<const protocol::WorldItemState> items);
		void save_property_catalog(const protocol::PropertyCatalog &catalog);
		void save_property_ledger(const protocol::PropertyLedger &ledger);

	private:
		void load_or_create(const server_config &config);
		void load_profiles();
		void load_world_objects();
		void load_world_items();
		void load_property_catalog();
		void load_property_ledger();
		void write_manifest() const;
		void write_profile(const persisted_profile &profile) const;

		std::filesystem::path m_root;
		std::filesystem::path m_profiles_directory;
		session_manifest m_manifest;
		std::vector<persisted_profile> m_profiles;
		std::vector<protocol::WorldObjectState> m_world_objects;
		std::vector<protocol::WorldItemState> m_world_items;
		protocol::PropertyCatalog m_property_catalog;
		protocol::PropertyLedger m_property_ledger;
	};
}
