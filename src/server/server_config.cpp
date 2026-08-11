#include "server/server_config.hpp"

#include "multiplayer/protocol.hpp"

#include <toml++/toml.hpp>

#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <regex>
#include <stdexcept>
#include <unordered_set>

namespace kcd2o::server
{
	namespace
	{
		template<typename Target>
		Target checked_integer(
		    const toml::table &table,
		    std::string_view key,
		    Target fallback)
		{
			const auto raw = table[key].value<std::int64_t>();
			if (!raw)
			{
				return fallback;
			}
			if (*raw < 0
			    || static_cast<std::uint64_t>(*raw)
			        > static_cast<std::uint64_t>(std::numeric_limits<Target>::max()))
			{
				throw std::runtime_error("server.toml value is out of range: "
				    + std::string(key));
			}
			return static_cast<Target>(*raw);
		}

		void load_catalog_ids(
		    const std::filesystem::path &path,
		    std::unordered_set<std::string> &output)
		{
			std::ifstream input(path, std::ios::binary);
			if (!input)
				return;
			const std::string text{
			    std::istreambuf_iterator<char>(input),
			    std::istreambuf_iterator<char>()};
			if (!text.contains(
			        "\"retail_build\": \"1308617_856\"")
			    || !text.contains(
			        "\"catalog_fingerprint\": \"22f4d6dc5438ecab\""))
			{
				return;
			}
			const std::regex soul_id(
			    R"catalog("soul_id"\s*:\s*"([0-9a-fA-F-]{36})")catalog");
			for (auto iterator =
			         std::sregex_iterator(text.begin(), text.end(), soul_id);
			     iterator != std::sregex_iterator();
			     ++iterator)
			{
				output.insert((*iterator)[1].str());
			}
		}
	}

	server_config load_server_config(const std::filesystem::path &path)
	{
		const auto document = toml::parse_file(path.string());
		const auto *server = document["server"].as_table();
		if (!server)
		{
			throw std::runtime_error("server.toml is missing the [server] table");
		}

		server_config config;
		config.bind_address = (*server)["bind_address"].value_or(config.bind_address);
		config.port = checked_integer(*server, "port", config.port);
		config.name = (*server)["name"].value_or(config.name);
		config.password = (*server)["password"].value_or(std::string{});
		config.max_players = checked_integer(*server, "max_players", config.max_players);
		if (const auto *auth = document["auth"].as_table())
		{
			config.account_auth_enabled = (*auth)["enabled"].value_or(false);
			config.account_whitelist_enabled =
			    (*auth)["whitelist_enabled"].value_or(false);
			config.account_service_url = (*auth)["service_url"].value_or(
			    config.account_service_url);
			config.account_server_id = (*auth)["server_id"].value_or(std::string{});
			config.public_address = (*auth)["public_address"].value_or(std::string{});
			config.account_identity_file = (*auth)["identity_file"].value_or(
			    config.account_identity_file.string());
			if (const auto key = (*auth)["server_key"].value<std::string>())
				config.account_server_key = *key;
			if (const auto key_file = (*auth)["server_key_file"].value<std::string>())
			{
				auto resolved = std::filesystem::path(*key_file);
				if (resolved.is_relative())
					resolved = std::filesystem::absolute(path).parent_path() / resolved;
				std::ifstream input(resolved);
				std::getline(input, config.account_server_key);
				if (config.account_server_key.empty())
					throw std::runtime_error("could not read [auth].server_key_file");
			}
		}
		if (config.account_identity_file.is_relative())
			config.account_identity_file =
			    std::filesystem::absolute(path).parent_path()
			    / config.account_identity_file;
		config.level_id = (*server)["level_id"].value_or(std::string{});
		config.required_content_hash =
		    (*server)["required_content_hash"].value_or(std::string{});
		config.tick_rate = checked_integer(*server, "tick_rate", config.tick_rate);
		config.snapshot_rate =
		    checked_integer(*server, "snapshot_rate", config.snapshot_rate);
		config.handshake_timeout_seconds = checked_integer(
		    *server,
		    "handshake_timeout_seconds",
		    config.handshake_timeout_seconds);
		config.idle_timeout_seconds = checked_integer(
		    *server,
		    "idle_timeout_seconds",
		    config.idle_timeout_seconds);
		config.reconnect_grace_seconds = checked_integer(
		    *server,
		    "reconnect_grace_seconds",
		    config.reconnect_grace_seconds);
		config.bootstrap_timeout_seconds = checked_integer(
		    *server,
		    "bootstrap_timeout_seconds",
		    config.bootstrap_timeout_seconds);
		config.profile_snapshot_interval_seconds = checked_integer(
		    *server,
		    "profile_snapshot_interval_seconds",
		    config.profile_snapshot_interval_seconds);
		if (const auto *environment = document["environment"].as_table())
		{
			config.initial_time_of_day_hours =
			    (*environment)["initial_time_of_day_hours"].value_or(
			        config.initial_time_of_day_hours);
			config.time_scale = static_cast<float>(
			    (*environment)["time_scale"].value_or(
			        static_cast<double>(config.time_scale)));
			config.weather_id = checked_integer(
			    *environment,
			    "weather_id",
			    config.weather_id);
			config.weather_transition_seconds = checked_integer(
			    *environment,
			    "weather_transition_seconds",
			    config.weather_transition_seconds);
			config.sleeping_players_required = checked_integer(
			    *environment,
			    "sleeping_players_required",
			    config.sleeping_players_required);
			config.sleep_wake_hour =
			    (*environment)["sleep_wake_hour"].value_or(
			        config.sleep_wake_hour);
		}
		// The aggregate key remains a backwards-compatible fallback. Explicit
		// category keys override it independently.
		const auto disable_all_npcs =
		    (*server)["disable_non_player_entities"].value_or(false);
		config.disable_human_npcs =
		    (*server)["disable_human_npcs"].value_or(disable_all_npcs);
		config.disable_animal_npcs =
		    (*server)["disable_animal_npcs"].value_or(disable_all_npcs);
		config.default_avatar_archetype =
		    (*server)["default_avatar_archetype"].value_or(
		        config.default_avatar_archetype);
		if (const auto *allowed =
		        (*server)["allowed_avatar_archetypes"].as_array())
		{
			config.allowed_avatar_archetypes.clear();
			for (const auto &entry : *allowed)
			{
				const auto value = entry.value<std::string>();
				if (!value)
				{
					throw std::runtime_error(
					    "allowed_avatar_archetypes must contain only strings");
				}
				config.allowed_avatar_archetypes.push_back(*value);
			}
		}
		load_catalog_ids(
		    std::filesystem::absolute(path).parent_path()
		        / "npc_archetypes.json",
		    config.known_avatar_archetypes);
		config.max_player_speed_mps =
		    (*server)["max_player_speed_mps"].value_or(config.max_player_speed_mps);
		config.movement_tolerance_m =
		    (*server)["movement_tolerance_m"].value_or(config.movement_tolerance_m);
		if (const auto *chat = document["chat"].as_table())
		{
			config.chat_whisper_range_m = static_cast<float>(
			    (*chat)["whisper_range_m"].value_or(
			        static_cast<double>(config.chat_whisper_range_m)));
			config.chat_say_range_m = static_cast<float>(
			    (*chat)["say_range_m"].value_or(
			        static_cast<double>(config.chat_say_range_m)));
			config.chat_shout_range_m = static_cast<float>(
			    (*chat)["shout_range_m"].value_or(
			        static_cast<double>(config.chat_shout_range_m)));
			config.chat_ooc_enabled = (*chat)["ooc_enabled"].value_or(true);
		}
		if (const auto *voice = document["voice"].as_table())
		{
			config.voice_enabled = (*voice)["enabled"].value_or(true);
			config.voice_whisper_range_m = static_cast<float>(
			    (*voice)["whisper_range_m"].value_or(
			        static_cast<double>(config.voice_whisper_range_m)));
			config.voice_normal_range_m = static_cast<float>(
			    (*voice)["normal_range_m"].value_or(
			        static_cast<double>(config.voice_normal_range_m)));
			config.voice_shout_range_m = static_cast<float>(
			    (*voice)["shout_range_m"].value_or(
			        static_cast<double>(config.voice_shout_range_m)));
			config.voice_max_frames_per_second = checked_integer(
			    *voice,
			    "max_frames_per_second",
			    config.voice_max_frames_per_second);
		}
		if (const auto *permissions = document["permissions"].as_table())
		{
			if (const auto *owners = (*permissions)["owners"].as_array())
			{
				for (const auto &owner : *owners)
				{
					const auto value = owner.value<std::string>();
					if (!value)
						throw std::runtime_error("[permissions].owners must contain only UUID strings");
					config.permission_owners.push_back(*value);
				}
			}
		}
		if (const auto *resources = document["resources"].as_table())
		{
			config.resources_enabled = (*resources)["enabled"].value_or(true);
			config.resource_directory = (*resources)["directory"].value_or(
			    config.resource_directory.string());
			config.script_memory_limit_mb = checked_integer(
			    *resources, "memory_limit_mb", config.script_memory_limit_mb);
			config.script_instruction_limit = checked_integer(
			    *resources, "instruction_limit", config.script_instruction_limit);
			config.script_error_limit = checked_integer(
			    *resources, "error_limit", config.script_error_limit);
		}
		config.world_directory =
		    (*server)["world_directory"].value_or(config.world_directory.string());
		config.starter_profile_path =
		    (*server)["starter_profile"].value_or(
		        config.starter_profile_path.string());
		config.npc_world_catalog_path =
		    std::filesystem::absolute(path).parent_path()
		    / "game_data" / "npc_world_catalog.json";
		if (const auto *property = document["property"].as_table())
		{
			if (property->contains("game_root"))
			{
				throw std::runtime_error(
				    "[property].game_root is obsolete; configure "
				    "[property].game_data instead");
			}
			config.property_game_data =
			    (*property)["game_data"].value_or(std::string{});
		}
		if (config.world_directory.is_relative())
		{
			config.world_directory =
			    std::filesystem::absolute(path).parent_path()
			    / config.world_directory;
		}
		if (config.starter_profile_path.is_relative())
		{
			config.starter_profile_path =
			    std::filesystem::absolute(path).parent_path()
			    / config.starter_profile_path;
		}
		if (config.resource_directory.is_relative())
		{
			config.resource_directory =
			    std::filesystem::absolute(path).parent_path()
			    / config.resource_directory;
		}
		if (!config.property_game_data.empty()
		    && config.property_game_data.is_relative())
		{
			config.property_game_data =
			    std::filesystem::absolute(path).parent_path()
			    / config.property_game_data;
		}
		config.starter_profile =
		    load_starter_profile_template(config.starter_profile_path);
		if (const auto *spawn = (*server)["initial_spawn"].as_table())
		{
			const auto x = (*spawn)["x"].value<double>();
			const auto y = (*spawn)["y"].value<double>();
			const auto z = (*spawn)["z"].value<double>();
			const auto qx = (*spawn)["qx"].value<double>();
			const auto qy = (*spawn)["qy"].value<double>();
			const auto qz = (*spawn)["qz"].value<double>();
			const auto qw = (*spawn)["qw"].value<double>();
			if (!x || !y || !z || !qx || !qy || !qz || !qw)
			{
				throw std::runtime_error(
				    "[server.initial_spawn] requires x, y, z, qx, qy, qz, and qw");
			}
			config.initial_spawn = initial_spawn_config{
			    static_cast<float>(*x),
			    static_cast<float>(*y),
			    static_cast<float>(*z),
			    static_cast<float>(*qx),
			    static_cast<float>(*qy),
			    static_cast<float>(*qz),
			    static_cast<float>(*qw)};
		}
		normalize_avatar_config(config);
		validate_server_config(config);
		return config;
	}

	void normalize_avatar_config(server_config &config)
	{
		config.known_avatar_archetypes.insert(
		    std::string(npc::default_soul_id));
		if (!config.known_avatar_archetypes.contains(
		        config.default_avatar_archetype))
		{
			config.default_avatar_archetype =
			    std::string(npc::default_soul_id);
		}

		std::vector<std::string> normalized;
		normalized.reserve(config.allowed_avatar_archetypes.size() + 1);
		std::unordered_set<std::string> seen;
		for (const auto &archetype : config.allowed_avatar_archetypes)
		{
			const auto value = config.known_avatar_archetypes.contains(archetype)
			    ? archetype
			    : config.default_avatar_archetype;
			if (seen.insert(value).second)
				normalized.push_back(value);
		}
		if (seen.insert(config.default_avatar_archetype).second)
			normalized.push_back(config.default_avatar_archetype);
		if (seen.insert(std::string(npc::default_soul_id)).second)
			normalized.emplace_back(npc::default_soul_id);
		config.allowed_avatar_archetypes = std::move(normalized);
	}

	void validate_server_config(const server_config &config)
	{
		if (config.bind_address.empty())
		{
			throw std::runtime_error("bind_address must not be empty");
		}
		if (config.port == 0)
		{
			throw std::runtime_error("port must be between 1 and 65535");
		}
		if (config.name.empty() || config.name.size() > 64)
		{
			throw std::runtime_error("server name must contain 1 to 64 bytes");
		}
		if (config.max_players == 0)
		{
			throw std::runtime_error("max_players must be at least 1");
		}
		if (config.account_auth_enabled
		    && (config.account_service_url.empty()
		        || config.account_server_id.size() > 64
		        || config.account_server_key.size() > 128
		        || config.account_server_id.empty() != config.account_server_key.empty()
		        || config.account_identity_file.empty()
		        || config.public_address.empty()
		        || config.public_address.size() > 256))
		{
			throw std::runtime_error(
			    "enabled [auth] requires service_url, identity_file, and public_address; explicit server_id and key must be provided together");
		}
		if (config.account_whitelist_enabled && !config.account_auth_enabled)
		{
			throw std::runtime_error(
			    "[auth].whitelist_enabled requires [auth].enabled");
		}
		if (config.level_id.empty() || config.level_id.size() > 128)
		{
			throw std::runtime_error("level_id must contain 1 to 128 bytes");
		}
		if (config.tick_rate < 10 || config.tick_rate > 120)
		{
			throw std::runtime_error("tick_rate must be between 10 and 120");
		}
		if (config.snapshot_rate == 0 || config.snapshot_rate > config.tick_rate)
		{
			throw std::runtime_error(
			    "snapshot_rate must be positive and no greater than tick_rate");
		}
		if (config.handshake_timeout_seconds == 0
		    || config.idle_timeout_seconds == 0
		    || config.reconnect_grace_seconds == 0
		    || config.bootstrap_timeout_seconds < 30
		    || config.bootstrap_timeout_seconds > 600
		    || config.profile_snapshot_interval_seconds < 5
		    || config.profile_snapshot_interval_seconds > 60)
		{
			throw std::runtime_error("timeouts and profile interval are invalid");
		}
		if (!std::isfinite(config.max_player_speed_mps)
		    || config.max_player_speed_mps <= 0.0F
		    || !std::isfinite(config.movement_tolerance_m)
		    || config.movement_tolerance_m < 0.0F)
		{
			throw std::runtime_error("movement limits must be finite and valid");
		}
		if (!std::isfinite(config.chat_whisper_range_m)
		    || !std::isfinite(config.chat_say_range_m)
		    || !std::isfinite(config.chat_shout_range_m)
		    || config.chat_whisper_range_m <= 0.0F
		    || config.chat_say_range_m < config.chat_whisper_range_m
		    || config.chat_shout_range_m < config.chat_say_range_m
		    || config.chat_shout_range_m > 250.0F)
		{
			throw std::runtime_error(
			    "chat ranges must be finite, positive, ordered, and no greater than 250m");
		}
		if (!std::isfinite(config.voice_whisper_range_m)
		    || !std::isfinite(config.voice_normal_range_m)
		    || !std::isfinite(config.voice_shout_range_m)
		    || config.voice_whisper_range_m <= 0.0F
		    || config.voice_normal_range_m < config.voice_whisper_range_m
		    || config.voice_shout_range_m < config.voice_normal_range_m
		    || config.voice_shout_range_m > 250.0F
		    || config.voice_max_frames_per_second < 50
		    || config.voice_max_frames_per_second > 100)
		{
			throw std::runtime_error(
			    "voice ranges or frame-rate limit are invalid");
		}
		for (const auto &owner : config.permission_owners)
		{
			if (!is_uuid(owner))
				throw std::runtime_error("[permissions].owners contains an invalid UUID");
		}
		if (!std::isfinite(config.initial_time_of_day_hours)
		    || config.initial_time_of_day_hours < 0.0
		    || config.initial_time_of_day_hours >= hours_per_day
		    || !std::isfinite(config.time_scale) || config.time_scale < 0.0F
		    || config.time_scale > maximum_time_scale
		    || config.weather_id < minimum_weather_id
		    || config.weather_id > maximum_weather_id
		    || config.weather_transition_seconds
		        > maximum_weather_transition_ms / 1000U
		    || config.sleeping_players_required == 0
		    || config.sleeping_players_required > config.max_players
		    || !std::isfinite(config.sleep_wake_hour)
		    || config.sleep_wake_hour < 0.0
		    || config.sleep_wake_hour >= hours_per_day)
		{
			throw std::runtime_error(
			    "environment time, scale, weather, sleep quorum, or transition is invalid");
		}
		if (config.world_directory.empty())
		{
			throw std::runtime_error("world_directory must not be empty");
		}
		if (config.resources_enabled && config.resource_directory.empty())
			throw std::runtime_error("[resources].directory must not be empty");
		if (config.script_memory_limit_mb < 4
		    || config.script_memory_limit_mb > 256
		    || config.script_instruction_limit < 10'000
		    || config.script_instruction_limit > 10'000'000
		    || config.script_error_limit == 0
		    || config.script_error_limit > 100)
		{
			throw std::runtime_error("resource script limits are invalid");
		}
		validate_starter_profile_template(config.starter_profile);
		protocol::AvatarPolicy avatar_policy;
		avatar_policy.set_default_archetype_id(
		    config.default_avatar_archetype);
		for (const auto &archetype : config.allowed_avatar_archetypes)
		{
			avatar_policy.add_allowed_archetype_ids(archetype);
		}
		if (!is_valid_avatar_policy(avatar_policy))
		{
			throw std::runtime_error(
			    "avatar archetype policy is empty, duplicated, invalid, "
			    "or does not include default_avatar_archetype");
		}
		if (config.initial_spawn)
		{
			const auto &spawn = *config.initial_spawn;
			const auto finite = std::isfinite(spawn.x) && std::isfinite(spawn.y)
			    && std::isfinite(spawn.z) && std::isfinite(spawn.qx)
			    && std::isfinite(spawn.qy) && std::isfinite(spawn.qz)
			    && std::isfinite(spawn.qw);
			const auto length = std::sqrt(
			    spawn.qx * spawn.qx + spawn.qy * spawn.qy
			    + spawn.qz * spawn.qz + spawn.qw * spawn.qw);
			if (!finite || length < 0.999F || length > 1.001F)
			{
				throw std::runtime_error(
				    "initial_spawn must be finite with a normalized quaternion");
			}
		}
	}
}
