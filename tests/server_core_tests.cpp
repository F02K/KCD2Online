#include "property/catalog.hpp"
#include "server/server_core.hpp"

#include <Windows.h>

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <exception>
#include <string>

#undef assert
#define assert(expression)                                                   \
	do                                                                       \
	{                                                                        \
		if (!(expression))                                                   \
		{                                                                    \
			std::cerr << "assertion failed at " << __FILE__ << ':' << __LINE__ \
			          << ": " #expression << '\n';                          \
			std::_Exit(1);                                                   \
		}                                                                    \
	} while (false)

namespace
{
	using namespace std::chrono_literals;
	using namespace kcd2o;
	using namespace kcd2o::server;

	struct temporary_world
	{
		temporary_world()
		{
			static std::uint32_t next_id{};
			path = std::filesystem::temp_directory_path()
			    / ("kcd2o-server-tests-"
			        + std::to_string(GetCurrentProcessId()) + "-"
			        + std::to_string(GetTickCount64()) + "-"
			        + std::to_string(++next_id));
			std::filesystem::create_directories(path);
		}

		~temporary_world()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}

		std::filesystem::path path;
	};

	server_config config_for(
	    const std::filesystem::path &world,
	    bool configured_spawn = true)
	{
		server_config config;
		config.name = "Sandbox Test";
		config.password = "secret";
		config.level_id = "sandbox";
		config.world_directory = world;
		if (configured_spawn)
		{
			config.initial_spawn = initial_spawn_config{
			    10.0F, 20.0F, 30.0F, 0.0F, 0.0F, 0.0F, 1.0F};
		}
		return config;
	}

	protocol::Envelope hello(std::string name = "Henry")
	{
		protocol::Envelope envelope;
		auto *message = envelope.mutable_client_hello();
		message->set_version(kcd2o_version);
		message->set_whgame_timestamp(supported_whgame_timestamp);
		message->set_whgame_image_size(supported_whgame_image_size);
		message->set_display_name(std::move(name));
		message->set_password("secret");
		auto *runtime = message->mutable_runtime();
		runtime->set_features(required_client_runtime_capabilities);
		runtime->set_kcse_version(1);
		runtime->set_game_version(0x01050600);
		runtime->set_release_index(1);
		runtime->set_runtime_epoch(1);
		const auto &address_library = supported_address_libraries.back();
		runtime->set_address_library(address_library.build_key);
		runtime->set_address_library_distribution(
		    address_library.distribution);
		runtime->set_address_library_format(address_library.format_version);
		runtime->set_address_library_entries(address_library.entry_count);
		runtime->set_address_library_sha256(address_library.sha256);
		return envelope;
	}

	protocol::Envelope enroll()
	{
		protocol::Envelope envelope;
		envelope.mutable_client_authenticate()->set_enroll(true);
		return envelope;
	}

	protocol::Envelope authenticate(std::string token)
	{
		protocol::Envelope envelope;
		envelope.mutable_client_authenticate()->set_identity_token(
		    std::move(token));
		return envelope;
	}

	protocol::Envelope claim(std::string code)
	{
		protocol::Envelope envelope;
		envelope.mutable_client_authenticate()->set_claim_code(
		    std::move(code));
		return envelope;
	}

	protocol::Envelope central_auth(std::string access_token)
	{
		protocol::Envelope envelope;
		envelope.mutable_client_authenticate()->set_access_token(
		    std::move(access_token));
		return envelope;
	}

	protocol::Envelope ready(const protocol::ServerBootstrap &bootstrap)
	{
		protocol::Envelope envelope;
		auto *message = envelope.mutable_client_world_ready();
		message->set_session_id(bootstrap.session_id());
		message->set_manifest_revision(bootstrap.manifest_revision());
		message->set_level_id(bootstrap.level_id());
		assert(bootstrap.profile().has_avatar());
		*message->mutable_avatar() = bootstrap.profile().avatar();
		return envelope;
	}

	protocol::Envelope client_transform(
	    std::uint64_t sequence,
	    float x = 10.0F)
	{
		protocol::Envelope envelope;
		auto *transform =
		    envelope.mutable_client_transform()->mutable_transform();
		transform->mutable_position()->set_x(x);
		transform->mutable_position()->set_y(20.0F);
		transform->mutable_position()->set_z(30.0F);
		transform->mutable_rotation()->set_w(1.0F);
		transform->mutable_velocity();
		transform->set_sequence(sequence);
		transform->set_client_time_ms(sequence * 10);
		return envelope;
	}

	const protocol::ServerBootstrap &find_bootstrap(
	    const std::vector<outbound_message> &messages,
	    connection_id connection)
	{
		for (const auto &message : messages)
		{
			if (message.connection == connection
			    && message.envelope.has_server_bootstrap())
			{
				return message.envelope.server_bootstrap();
			}
		}
		std::cerr << "missing bootstrap for connection " << connection
		          << "; outbound payloads:";
		for (const auto &message : messages)
		{
			std::cerr << ' ' << message.connection << ':'
			          << static_cast<int>(message.envelope.payload_case());
			if (message.envelope.has_server_rejected())
			{
				std::cerr << '('
				          << message.envelope.server_rejected().message()
				          << ')';
			}
		}
		std::cerr << '\n';
		assert(false);
		std::abort();
	}

	bool has_accepted(
	    const std::vector<outbound_message> &messages,
	    connection_id connection,
	    player_id expected)
	{
		for (const auto &message : messages)
		{
			if (message.connection == connection
			    && message.envelope.has_server_accepted()
			    && message.envelope.server_accepted().player_id() == expected)
			{
				return true;
			}
		}
		return false;
	}

	bool has_rejection(
	    const std::vector<outbound_message> &messages,
	    connection_id connection,
	    protocol::RejectReason reason)
	{
		return std::ranges::any_of(
		    messages,
		    [&](const outbound_message &message)
		    {
			    return message.connection == connection
			        && message.envelope.has_server_rejected()
			        && message.envelope.server_rejected().reason() == reason;
		    });
	}

	bool has_entity_control(
	    const std::vector<outbound_message> &messages,
	    connection_id connection,
	    bool humans_disabled,
	    bool animals_disabled)
	{
		return std::ranges::any_of(
		    messages,
		    [&](const outbound_message &message)
			{
				if (message.connection != connection
				    || !message.envelope.has_server_entity_control())
					return false;
				const auto &control =
				    message.envelope.server_entity_control();
				return control.has_human_npcs_disabled()
				    && control.human_npcs_disabled() == humans_disabled
				    && control.has_animal_npcs_disabled()
				    && control.animal_npcs_disabled() == animals_disabled
				    && control.non_player_entities_disabled()
				        == (humans_disabled && animals_disabled);
			});
	}

	bool has_system_chat(
	    const std::vector<outbound_message> &messages,
	    connection_id connection,
	    std::string_view text)
	{
		return std::ranges::any_of(
		    messages,
		    [&](const outbound_message &message)
		    {
			    return message.connection == connection
			        && message.delivery == reliability::reliable
			        && message.envelope.has_chat_broadcast()
			        && message.envelope.chat_broadcast().player_id() == 0
			        && message.envelope.chat_broadcast().display_name()
			            == "Server"
			        && message.envelope.chat_broadcast().text() == text;
		    });
	}

	std::string connect_new_player(
	    server_core &core,
	    connection_id connection,
	    time_point now,
	    player_id expected_id,
	    protocol::PlayerProfile *enrolled_profile = nullptr,
	    std::string name = "Henry")
	{
		const auto expected_name = name;
		core.on_transport_connected(connection, now);
		core.on_message(connection, hello(std::move(name)), now);
		auto outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.has_server_challenge());

		core.on_message(connection, enroll(), now + 1ms);
		outbound = core.take_outbound();
		const auto bootstrap = find_bootstrap(outbound, connection);
		assert(bootstrap.mode() == protocol::BOOTSTRAP_MODE_LOAD);
		assert(bootstrap.has_environment());
		assert(is_valid_environment_state(bootstrap.environment()));
		assert(!bootstrap.issued_identity_token().empty());
		const auto token = bootstrap.issued_identity_token();
		if (enrolled_profile)
			*enrolled_profile = bootstrap.profile();

		core.on_message(connection, ready(bootstrap), now + 2ms);
		outbound = core.take_outbound();
		assert(has_accepted(outbound, connection, expected_id));
		assert(has_entity_control(
		    outbound,
		    connection,
		    core.human_npcs_disabled(),
		    core.animal_npcs_disabled()));
		const auto other_players = core.players().size() - 1;
		assert(std::ranges::count_if(
		    outbound,
		    [&](const outbound_message &message)
		    {
			    return message.connection != connection
			        && message.envelope.has_chat_broadcast()
			        && message.envelope.chat_broadcast().player_id() == 0
			        && message.envelope.chat_broadcast().display_name()
			            == "Server"
			        && message.envelope.chat_broadcast().text()
			            == expected_name + " joined the server.";
		    }) == other_players);
		assert(!has_system_chat(
		    outbound,
		    connection,
		    expected_name + " joined the server."));
		return token;
	}
}

int main()
{
	std::set_terminate(
	    []
	    {
		    if (const auto exception = std::current_exception())
		    {
			    try
			    {
				    std::rethrow_exception(exception);
			    }
			    catch (const std::exception &error)
			    {
				    std::cerr << "unhandled exception: " << error.what()
				              << '\n';
			    }
		    }
		    std::_Exit(1);
	    });
	using namespace kcd2o;
	using namespace kcd2o::server;
	const auto start = clock::now();

	temporary_world parsed_config_world;
	{
		const auto path = parsed_config_world.path / "server.toml";
		std::filesystem::copy_file(
		    std::filesystem::path(KCD2Online_SOURCE_DIR) / "starter_profile.toml",
		    parsed_config_world.path / "starter_profile.toml");
		std::ofstream output(path);
		output
		    << "[server]\n"
		       "level_id = \"sandbox\"\n"
		       "max_players = 50000\n"
		       "world_directory = \"world\"\n"
		       "disable_non_player_entities = true\n"
		       "[auth]\n"
		       "enabled = true\n"
		       "service_url = \"https://api.kingdom-online.cc\"\n"
		       "identity_file = \"server-identity.json\"\n"
		       "public_address = \"203.0.113.20:27020\"\n"
		       "[property]\n"
		       "game_data = \"generated-game-data\"\n"
		       "[environment]\n"
		       "initial_time_of_day_hours = 21.5\n"
		       "time_scale = 30.0\n"
		       "weather_id = 8\n"
		       "weather_transition_seconds = 12\n"
		       "sleeping_players_required = 3\n"
		       "sleep_wake_hour = 7.25\n";
		output.close();
		const auto parsed = load_server_config(path);
		assert(parsed.disable_human_npcs);
		assert(parsed.max_players == 50'000);
		assert(parsed.disable_animal_npcs);
		assert(parsed.world_directory
		    == parsed_config_world.path / "world");
		assert(parsed.property_game_data
		    == parsed_config_world.path / "generated-game-data");
		assert(parsed.initial_time_of_day_hours == 21.5);
		assert(parsed.time_scale == 30.0F);
		assert(parsed.weather_id == 8);
		assert(parsed.weather_transition_seconds == 12);
		assert(parsed.sleeping_players_required == 3);
		assert(parsed.sleep_wake_hour == 7.25);
		assert(parsed.account_auth_enabled);
		assert(parsed.account_server_id.empty());
		assert(parsed.account_server_key.empty());
		assert(parsed.account_identity_file
		    == parsed_config_world.path / "server-identity.json");
	}

	temporary_world generated_property_world;
	temporary_world central_auth_world;
	{
		auto config = config_for(central_auth_world.path);
		config.account_auth_enabled = true;
		config.account_service_url = "https://api.kingdom-online.cc";
		config.account_server_id = "central-test";
		config.account_server_key = "test-key";
		config.public_address = "127.0.0.1:27020";
		constexpr std::string_view account_id =
		    "0c997ac1-8ae3-45b0-9b7f-bf3bd45ea21e";
		server_core core(
		    config,
		    {},
		    [&](std::string_view token, std::string &error)
		    {
			    if (token == "valid-access-token")
				    return std::optional{std::string(account_id)};
			    error = "invalid central token";
			    return std::optional<std::string>{};
		    });
		core.on_transport_connected(90, start);
		core.on_message(90, hello(), start);
		auto outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.server_challenge().central_auth_required());
		assert(outbound.front().envelope.server_challenge().server_id()
		    == "central-test");
		core.on_message(90, central_auth("invalid"), start + 1ms);
		outbound = core.take_outbound();
		assert(has_rejection(outbound, 90, protocol::REJECT_REASON_IDENTITY_REQUIRED));

		core.on_transport_connected(91, start + 2ms);
		core.on_message(91, hello(), start + 2ms);
		(void)core.take_outbound();
		core.on_message(91, central_auth("valid-access-token"), start + 3ms);
		outbound = core.take_outbound();
		const auto bootstrap = find_bootstrap(outbound, 91);
		assert(bootstrap.issued_identity_token().empty());
		assert(bootstrap.profile().persistent_id() == account_id);
		core.on_message(91, ready(bootstrap), start + 4ms);
		outbound = core.take_outbound();
		assert(has_accepted(outbound, 91, 1));
		assert(core.players().front().persistent_id == account_id);
	}

	{
		const auto game_data = generated_property_world.path / "game_data";
		std::filesystem::create_directories(game_data);
		protocol::PropertyCatalog generated;
		generated.set_schema(property::catalog_schema);
		generated.set_level_id("sandbox");
		generated.set_content_fingerprint("generated-fixture");
		auto *definition = generated.add_properties();
		definition->set_property_id("sandbox:fixture");
		definition->set_level_id("sandbox");
		definition->set_anchor_guid("00000001-0000-0000");
		definition->set_inferred_name("Generated fixture");
		definition->set_source_path("fixture/generated_home");
		definition->set_discovery_confidence(1.0F);
		definition->set_marker_entity_guid(1);
		definition->mutable_marker_position()->set_x(1.0F);
		definition->mutable_marker_position()->set_y(2.0F);
		definition->mutable_marker_position()->set_z(3.0F);
		{
			std::ofstream output(
			    game_data / "property_catalog_sandbox.pb",
			    std::ios::binary | std::ios::trunc);
			assert(output && generated.SerializeToOstream(&output));
		}

		auto config = config_for(generated_property_world.path / "world");
		config.property_game_data = game_data;
		server_core core(config);
		assert(core.property_catalog().properties_size() == 1);
		assert(core.property_catalog().properties(0).property_id()
		    == "sandbox:fixture");
		assert(std::filesystem::is_regular_file(
		    config.world_directory / "property_catalog.pb"));
	}

	temporary_world environment_world;
	{
		auto config = config_for(environment_world.path);
		config.initial_time_of_day_hours = 23.5;
		config.time_scale = 600.0F;
		config.weather_id = 2;
		config.weather_transition_seconds = 5;
		config.idle_timeout_seconds = 300;
		server_core core(config);
		(void)connect_new_player(core, 93, start, 1);
		auto state = core.current_environment(start + 126s);
		assert(std::abs(state.time_of_day_hours() - 20.5) < 0.000001);
		assert(state.time_scale() == 600.0F);
		assert(state.weather_id() == 2);
		const auto revision = state.revision();
		assert(core.set_time_scale(15.0F, start + 126s));
		auto outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().delivery == reliability::reliable);
		assert(outbound.front().envelope.has_server_environment_updated());
		state = core.current_environment(start + 126s);
		assert(state.revision() == revision + 1);
		assert(std::abs(state.time_of_day_hours() - 20.5) < 0.000001);
		assert(core.set_time_of_day(6.25, start + 126s));
		(void)core.take_outbound();
		assert(core.set_weather(13, 30, start + 126s));
		outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front()
		           .envelope.server_environment_updated()
		           .state()
		           .weather_id()
		    == 13);
		state = core.current_environment(start + 126s);
		assert(std::abs(state.time_of_day_hours() - 6.25) < 0.000001);
		assert(state.weather_id() == 13);
		assert(state.weather_transition_ms() == 30'000);
		assert(!core.set_weather(34, 30, start + 126s));
		core.tick(start + 127s);
		outbound = core.take_outbound();
		assert(std::ranges::any_of(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.delivery == reliability::unreliable
			        && message.envelope.has_world_snapshot()
			        && message.envelope.world_snapshot().has_environment()
			        && message.envelope.world_snapshot().environment().weather_id()
			            == 13;
		    }));
	}

	temporary_world sleep_and_respawn_world;
	{
		auto config = config_for(sleep_and_respawn_world.path);
		config.sleeping_players_required = 2;
		config.sleep_wake_hour = 6.5;
		server_core core(config);
		(void)connect_new_player(core, 94, start, 1, nullptr, "Henry");
		(void)connect_new_player(core, 95, start + 10ms, 2, nullptr, "Hans");

		protocol::Envelope sleep;
		sleep.mutable_client_sleep_state()->set_sleeping(true);
		core.on_message(94, sleep, start + 20ms);
		auto outbound = core.take_outbound();
		assert(std::ranges::count_if(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.envelope.has_server_sleep_state()
			        && message.envelope.server_sleep_state().sleeping_players() == 1
			        && message.envelope.server_sleep_state().required_players() == 2
			        && !message.envelope.server_sleep_state().time_skipped();
		    }) == 2);
		assert(std::ranges::none_of(
		    outbound,
		    [](const outbound_message &message)
		    { return message.envelope.has_server_environment_updated(); }));

		core.on_message(95, sleep, start + 21ms);
		outbound = core.take_outbound();
		assert(std::ranges::count_if(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.envelope.has_server_environment_updated()
			        && std::abs(message.envelope.server_environment_updated()
			                        .state()
			                        .time_of_day_hours()
			                    - 6.5)
			            < 0.000001;
		    }) == 2);
		assert(std::ranges::count_if(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.envelope.has_server_sleep_state()
			        && message.envelope.server_sleep_state().sleeping_players() == 0
			        && message.envelope.server_sleep_state().required_players() == 2
			        && message.envelope.server_sleep_state().time_skipped();
		    }) == 2);
		assert(has_system_chat(
		    outbound,
		    94,
		    "Time advanced because enough players went to sleep."));
		assert(has_system_chat(
		    outbound,
		    95,
		    "Time advanced because enough players went to sleep."));

		protocol::Envelope respawn;
		respawn.mutable_client_respawn_request();
		core.on_message(94, respawn, start + 22ms);
		assert(core.take_outbound().empty());

		protocol::Envelope death;
		death.mutable_client_death();
		core.on_message(94, death, start + 23ms);
		outbound = core.take_outbound();
		assert(has_system_chat(outbound, 94, "Henry died."));
		assert(has_system_chat(outbound, 95, "Henry died."));
		core.on_message(94, respawn, start + 24ms);
		outbound = core.take_outbound();
		assert(std::ranges::count_if(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.connection == 94
			        && message.envelope.has_server_respawn()
			        && message.envelope.server_respawn().spawn().position().x()
			            == 10.0F
			        && message.envelope.server_respawn().spawn().position().y()
			            == 20.0F
			        && message.envelope.server_respawn().spawn().position().z()
			            == 30.0F;
		    }) == 1);
		assert(has_system_chat(outbound, 94, "Henry respawned."));
		assert(has_system_chat(outbound, 95, "Henry respawned."));
		core.on_message(94, respawn, start + 25ms);
		assert(core.take_outbound().empty());
	}

	temporary_world lifecycle_chat_world;
	{
		server_core core(config_for(lifecycle_chat_world.path));
		(void)connect_new_player(core, 96, start, 1, nullptr, "Henry");
		const auto hans_token =
		    connect_new_player(core, 97, start + 10ms, 2, nullptr, "Hans");

		core.on_transport_disconnected(
		    97,
		    true,
		    "connection interrupted",
		    start + 20ms);
		auto outbound = core.take_outbound();
		assert(has_system_chat(
		    outbound,
		    96,
		    "Hans lost connection; waiting for reconnection."));
		assert(std::ranges::none_of(
		    outbound,
		    [](const outbound_message &message)
		    { return message.envelope.has_player_left(); }));

		core.on_transport_connected(98, start + 21ms);
		core.on_message(98, hello("Hans"), start + 21ms);
		(void)core.take_outbound();
		core.on_message(
		    98,
		    authenticate(hans_token),
		    start + 22ms);
		const auto bootstrap = find_bootstrap(core.take_outbound(), 98);
		core.on_message(98, ready(bootstrap), start + 23ms);
		outbound = core.take_outbound();
		assert(has_accepted(outbound, 98, 2));
		assert(has_system_chat(outbound, 96, "Hans reconnected."));
		assert(!has_system_chat(outbound, 98, "Hans reconnected."));

		core.on_transport_disconnected(
		    98,
		    false,
		    "client disconnected",
		    start + 24ms);
		outbound = core.take_outbound();
		assert(has_system_chat(outbound, 96, "Hans left the server."));
		assert(std::ranges::any_of(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.connection == 96
			        && message.envelope.has_player_left()
			        && message.envelope.player_left().player_id() == 2;
		    }));

		(void)connect_new_player(
		    core,
		    99,
		    start + 25ms,
		    3,
		    nullptr,
		    "Bob");
		core.kick(3, "server rule", start + 30ms);
		outbound = core.take_outbound();
		assert(has_system_chat(
		    outbound,
		    96,
		    "Bob was kicked from the server."));

		core.shutdown("maintenance");
		outbound = core.take_outbound();
		assert(has_system_chat(
		    outbound,
		    96,
		    "Server is shutting down."));
		assert(std::ranges::any_of(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.connection == 96
			        && message.envelope.has_server_shutdown()
			        && message.envelope.server_shutdown().reason()
			            == "maintenance";
		    }));
	}

	{
		const auto path = parsed_config_world.path / "split-server.toml";
		std::ofstream output(path);
		output
		    << "[server]\n"
		       "level_id = \"sandbox\"\n"
		       "world_directory = \"world\"\n"
		       "starter_profile = \"starter_profile.toml\"\n"
		       "disable_non_player_entities = true\n"
		       "disable_animal_npcs = false\n";
		output.close();
		const auto parsed = load_server_config(path);
		assert(parsed.disable_human_npcs);
		assert(!parsed.disable_animal_npcs);
	}

	temporary_world invalid_config_world;
	{
		auto invalid = config_for(invalid_config_world.path / "must-not-exist");
		invalid.max_players = 0;
		bool rejected = false;
		try
		{
			server_core core(invalid);
			(void)core;
		}
		catch (const std::exception &)
		{
			rejected = true;
		}
		assert(rejected);
		assert(!std::filesystem::exists(invalid.world_directory));
	}

	temporary_world incomplete_runtime_world;
	{
		server_core core(config_for(incomplete_runtime_world.path));
		core.on_transport_connected(89, start);
		auto wrong_version = hello();
		wrong_version.mutable_client_hello()->set_version("0.0.8");
		core.on_message(89, wrong_version, start);
		auto outbound = core.take_outbound();
		assert(has_rejection(
		    outbound,
		    89,
		    protocol::REJECT_REASON_VERSION_MISMATCH));

		core.on_transport_connected(90, start);
		auto missing_capability = hello();
		missing_capability.mutable_client_hello()
		    ->mutable_runtime()
		    ->set_features(runtime_capability_kcse);
		core.on_message(90, missing_capability, start);
		outbound = core.take_outbound();
		assert(has_rejection(
		    outbound,
		    90,
		    protocol::REJECT_REASON_GAME_BUILD_MISMATCH));

		core.on_transport_connected(92, start);
		auto wrong_address_library = hello();
		wrong_address_library.mutable_client_hello()
		    ->mutable_runtime()
		    ->set_address_library_sha256(std::string(64, '0'));
		core.on_message(92, wrong_address_library, start);
		outbound = core.take_outbound();
		assert(has_rejection(
		    outbound,
		    92,
		    protocol::REJECT_REASON_GAME_BUILD_MISMATCH));

		core.on_transport_connected(91, start);
		auto missing_address_library = hello();
		missing_address_library.mutable_client_hello()
		    ->mutable_runtime()
		    ->clear_address_library();
		core.on_message(91, missing_address_library, start);
		outbound = core.take_outbound();
		assert(has_rejection(
		    outbound,
		    91,
		    protocol::REJECT_REASON_GAME_BUILD_MISMATCH));
	}

	temporary_world persistent_world;
	std::string identity_token;
	player_id persistent_id{};
	std::string persistent_uuid;
	{
		auto counter = 0;
		protocol::PlayerProfile enrolled_profile;
		server_core core(
		    config_for(persistent_world.path),
		    [&]
		    {
			    return "test-token-" + std::to_string(++counter);
		    });
		identity_token = connect_new_player(
		    core,
		    1,
		    start,
		    1,
		    &enrolled_profile);
		persistent_id = core.players().front().id;
		persistent_uuid = core.players().front().persistent_id;
		assert(is_uuid(persistent_uuid));
		assert(enrolled_profile.persistent_id() == persistent_uuid);
		core.on_message(1, client_transform(100), start + 3ms);
		assert(core.players().front().last_sequence == 100);

		protocol::Envelope update;
		auto *profile_update = update.mutable_client_profile_update();
		profile_update->set_base_revision(enrolled_profile.revision());
		auto *profile = profile_update->mutable_profile();
		*profile = enrolled_profile;
		profile->set_money(profile->money() + 1);
		profile->set_money_subunits(7);
		core.on_message(1, update, start + 4ms);
		auto outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.profile_accepted().revision() == 2);

		core.on_transport_connected(2, start + 5ms);
		core.on_message(2, hello(), start + 5ms);
		(void)core.take_outbound();
		core.on_message(2, authenticate(identity_token), start + 6ms);
		outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.server_rejected().reason()
		    == protocol::REJECT_REASON_IDENTITY_IN_USE);

		const auto claim_code =
		    core.create_profile_claim(persistent_id, start + 7ms);
		assert(claim_code);
		core.on_transport_disconnected(
		    1,
		    false,
		    "intentional disconnect",
		    start + 8ms);
		(void)core.take_outbound();

		core.on_transport_connected(3, start + 9ms);
		core.on_message(3, hello(), start + 9ms);
		(void)core.take_outbound();
		core.on_message(3, claim(*claim_code), start + 10ms);
		outbound = core.take_outbound();
		const auto reclaimed = find_bootstrap(outbound, 3);
		assert(!reclaimed.issued_identity_token().empty());
		assert(reclaimed.profile().revision() == 2);
		assert(reclaimed.profile().money_subunits() == 7);
		assert(reclaimed.profile().persistent_id() == persistent_uuid);
		identity_token = reclaimed.issued_identity_token();
	}

	{
		server_core restarted(config_for(persistent_world.path));
		restarted.on_transport_connected(10, start + 1s);
		restarted.on_message(10, hello("Hans"), start + 1s);
		(void)restarted.take_outbound();
		restarted.on_message(
		    10,
		    authenticate(identity_token),
		    start + 1001ms);
		auto outbound = restarted.take_outbound();
		const auto bootstrap = find_bootstrap(outbound, 10);
		assert(bootstrap.profile().player_id() == persistent_id);
		assert(bootstrap.profile().revision() == 3);
		assert(bootstrap.profile().money_subunits() == 7);
		assert(bootstrap.profile().persistent_id() == persistent_uuid);
		assert(bootstrap.profile().display_name() == "Hans");
		restarted.on_message(10, ready(bootstrap), start + 1002ms);
		(void)restarted.take_outbound();
		restarted.on_message(
		    10,
		    client_transform(1),
		    start + 1003ms);
		assert(restarted.players().front().last_sequence == 1);
		assert(restarted.players().front().display_name == "Hans");
	}

	{
		server_core restarted(config_for(persistent_world.path));
		restarted.on_transport_connected(11, start + 2s);
		restarted.on_message(11, hello("Hans"), start + 2s);
		(void)restarted.take_outbound();
		restarted.on_message(
		    11,
		    authenticate(identity_token),
		    start + 2001ms);
		const auto bootstrap = find_bootstrap(restarted.take_outbound(), 11);
		assert(bootstrap.profile().player_id() == persistent_id);
		assert(bootstrap.profile().persistent_id() == persistent_uuid);
		assert(bootstrap.profile().display_name() == "Hans");
		assert(bootstrap.profile().revision() == 3);
	}

	temporary_world rename_collision_world;
	{
		server_core core(config_for(rename_collision_world.path));
		const auto alice_token =
		    connect_new_player(core, 20, start + 3s, 1, nullptr, "Alice");
		(void)connect_new_player(
		    core,
		    21,
		    start + 3003ms,
		    2,
		    nullptr,
		    "Bob");
		core.on_transport_disconnected(
		    20,
		    false,
		    "intentional disconnect",
		    start + 3006ms);
		(void)core.take_outbound();

		core.on_transport_connected(22, start + 3007ms);
		core.on_message(22, hello("Bob"), start + 3007ms);
		(void)core.take_outbound();
		core.on_message(
		    22,
		    authenticate(alice_token),
		    start + 3008ms);
		const auto outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.has_server_rejected());
		assert(outbound.front().envelope.server_rejected().reason()
		    == protocol::REJECT_REASON_IDENTITY_REQUIRED);
		assert(outbound.front().envelope.server_rejected().message()
		    == "display name is already in use");
	}

	temporary_world initializer_world;
	{
		auto counter = 0;
		server_core core(
		    config_for(initializer_world.path, false),
		    [&]
		    {
			    return "initializer-token-" + std::to_string(++counter);
		    });
		core.on_transport_connected(20, start);
		core.on_message(20, hello("Henry"), start);
		(void)core.take_outbound();
		core.on_message(20, enroll(), start + 1ms);
		auto outbound = core.take_outbound();
		const auto initializer = find_bootstrap(outbound, 20);
		assert(initializer.mode() == protocol::BOOTSTRAP_MODE_INITIALIZE);

		core.on_transport_connected(21, start + 2ms);
		core.on_message(21, hello("Hans"), start + 2ms);
		(void)core.take_outbound();
		core.on_message(21, enroll(), start + 3ms);
		outbound = core.take_outbound();
		assert(find_bootstrap(outbound, 21).mode()
		    == protocol::BOOTSTRAP_MODE_WAIT);

		auto initializer_ready = ready(initializer);
		auto *spawn =
		    initializer_ready.mutable_client_world_ready()
		        ->mutable_initial_spawn();
		spawn->mutable_position()->set_x(100.0F);
		spawn->mutable_position()->set_y(200.0F);
		spawn->mutable_position()->set_z(300.0F);
		spawn->mutable_rotation()->set_w(1.0F);
		spawn->mutable_velocity();
		initializer_ready.mutable_client_world_ready()
		    ->set_initialized_session(true);
		core.on_message(20, initializer_ready, start + 4ms);
		outbound = core.take_outbound();
		assert(has_accepted(outbound, 20, 1));
		assert(find_bootstrap(outbound, 21).mode()
		    == protocol::BOOTSTRAP_MODE_LOAD);
	}

	temporary_world capacity_world;
	{
		auto config = config_for(capacity_world.path);
		config.max_players = 1;
		server_core core(config);
		(void)connect_new_player(core, 30, start, 1);
		core.on_transport_connected(31, start + 1s);
		core.on_message(31, hello("Hans"), start + 1s);
		(void)core.take_outbound();
		core.on_message(31, enroll(), start + 1001ms);
		const auto outbound = core.take_outbound();
		assert(has_rejection(
		    outbound,
		    31,
		    protocol::REJECT_REASON_SERVER_FULL));
	}

	temporary_world entity_control_world;
	{
		auto config = config_for(entity_control_world.path);
		config.disable_human_npcs = true;
		config.disable_animal_npcs = false;
		server_core core(config);
		assert(core.human_npcs_disabled());
		assert(!core.animal_npcs_disabled());
		(void)connect_new_player(core, 35, start, 1);

		assert(!core.set_npc_entities_disabled(true, false));
		assert(core.take_outbound().empty());
		assert(core.set_npc_entities_disabled(false, true));
		auto outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(has_entity_control(outbound, 35, false, true));
		assert(!core.human_npcs_disabled());
		assert(core.animal_npcs_disabled());
		assert(!core.set_npc_entities_disabled(false, true));
		assert(core.take_outbound().empty());
	}

	temporary_world dummy_world;
	{
		server_core core(config_for(dummy_world.path));
		(void)connect_new_player(core, 36, start, 1);

		std::string error;
		const auto dummy_id =
		    core.spawn_dummy("Training Dummy", &error);
		assert(dummy_id);
		assert(error.empty());
		const auto players_with_dummy = core.players();
		assert(players_with_dummy.size() == 2);
		const auto dummy = std::ranges::find(
		    players_with_dummy,
		    *dummy_id,
		    &player_view::id);
		assert(dummy != players_with_dummy.end());
		assert(dummy->dummy);
		assert(dummy->connected);
		assert(dummy->has_transform);
		auto outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().connection == 36);
		assert(outbound.front().envelope.has_player_joined());
		const auto &joined =
		    outbound.front().envelope.player_joined().player();
		assert(joined.player_id() == *dummy_id);
		assert(joined.display_name() == "Training Dummy");
		assert(joined.connected());
		assert(joined.transform_valid());
		assert(joined.transform().position().x() == 12.0F);
		assert(joined.has_avatar());
		assert(
		    joined.avatar().archetype_id()
		    == core.config().default_avatar_archetype);
		assert(joined.avatar().revision() == 1);
		assert(encode(
		    outbound.front().envelope,
		    outbound.front().delivery));
		assert(!core.create_profile_claim(*dummy_id, start + 1ms));

		// Dummies wait for their spawn warm-up and then publish ordinary player
		// locomotion snapshots. Position stays anchored so only the native client
		// controller can move and animate the visual actor.
		core.tick(start + 100ms);
		(void)core.take_outbound();
		bool saw_dummy_motion = false;
		for (std::uint32_t step = 2; step <= 30; ++step)
		{
			core.tick(start + std::chrono::milliseconds(step * 100));
			outbound = core.take_outbound();
			for (const auto &message : outbound)
			{
				if (!message.envelope.has_world_snapshot())
					continue;
				for (const auto &snapshot :
				     message.envelope.world_snapshot().players())
				{
					if (snapshot.player_id() == *dummy_id
					    && snapshot.transform().sequence() > 1
					    && snapshot.movement_mode()
					        != protocol::MOVEMENT_MODE_IDLE)
					{
						assert(snapshot.transform().position().x() == 12.0F);
						assert(snapshot.transform().position().y() == 20.0F);
						assert(snapshot.transform().position().z() == 30.0F);
						const auto &velocity = snapshot.transform().velocity();
						const auto speed = std::sqrt(
						    velocity.x() * velocity.x()
						    + velocity.y() * velocity.y());
						assert(std::abs(speed - 1.5F) < 0.001F);
						const auto &rotation = snapshot.transform().rotation();
						const auto forward_x =
						    -2.0F * rotation.z() * rotation.w();
						const auto forward_y =
						    1.0F - 2.0F * rotation.z() * rotation.z();
						assert(
						    velocity.x() / speed * forward_x
						        + velocity.y() / speed * forward_y
						    > 0.999F);
						saw_dummy_motion = true;
					}
				}
			}
		}
		assert(saw_dummy_motion);

		assert(!core.spawn_dummy("Training Dummy", &error));
		assert(error == "display name is already in use");
		assert(core.take_outbound().empty());

		assert(core.remove_dummy(*dummy_id, start + 2ms));
		outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.has_player_left());
		assert(
		    outbound.front().envelope.player_left().player_id()
		    == *dummy_id);
		assert(core.players().size() == 1);
		assert(!core.remove_dummy(*dummy_id, start + 3ms));
	}

	temporary_world equipped_dummy_world;
	{
		auto config = config_for(equipped_dummy_world.path);
		config.starter_profile.inventory.push_back({
		    .definition_id = "b867dd0e-1bfe-40e9-b114-4b126a3ff1b0",
		    .count = 1,
		    .quality = 1.0F,
		    .condition = 1.0F,
		    .equipped_slot = "PrimaryMainHand"});
		server_core core(config);
		(void)connect_new_player(core, 136, start, 1);
		const auto dummy_id = core.spawn_dummy("Equipped Dummy");
		assert(dummy_id);
		const auto outbound = core.take_outbound();
		assert(outbound.size() == 1);
		const auto &avatar =
		    outbound.front().envelope.player_joined().player().avatar();
		assert(avatar.equipment_size() == 1);
		assert(
		    avatar.equipment(0).definition_id()
		    == "b867dd0e-1bfe-40e9-b114-4b126a3ff1b0");
		assert(
		    avatar.equipment(0).equipped_slot() == "PrimaryMainHand");
	}

	temporary_world avatar_world;
	{
		constexpr std::string_view knight_soul =
		    "11111111-2222-4333-8444-555555555555";
		auto config = config_for(avatar_world.path);
		config.known_avatar_archetypes.insert(std::string(knight_soul));
		config.allowed_avatar_archetypes.push_back(std::string(knight_soul));
		config.starter_profile.inventory.push_back({
		    .definition_id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
		    .count = 1,
		    .quality = 1.0F,
		    .condition = 1.0F,
		    .equipped_slot = "PrimaryMainHand"});
		server_core core(config);
		protocol::PlayerProfile enrolled_profile;
		(void)connect_new_player(
		    core,
		    37,
		    start,
		    1,
		    &enrolled_profile);

		protocol::Envelope update;
		auto *message = update.mutable_client_avatar_update();
		message->set_base_revision(1);
		auto *avatar = message->mutable_avatar();
		avatar->set_archetype_id(std::string(knight_soul));
		avatar->set_revision(1);
		avatar->set_stance(protocol::AVATAR_STANCE_READY);
		avatar->set_weapon_class(
		    protocol::AVATAR_WEAPON_CLASS_ONE_HANDED);
		avatar->set_weapon_drawn(true);
		avatar->set_active_weapon_set(
		    protocol::AVATAR_WEAPON_SET_PRIMARY);
		auto *item = avatar->add_equipment();
		item->set_definition_id(
		    "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
		item->set_equipped_slot("PrimaryMainHand");
		core.on_message(37, update, start + 4ms);
		auto outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.has_avatar_accepted());
		assert(outbound.front().envelope.avatar_accepted().revision() == 2);

		core.on_message(37, update, start + 5ms);
		outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.has_avatar_rejected());
		assert(outbound.front()
		           .envelope.avatar_rejected()
		           .authoritative_avatar()
		           .revision()
		    == 2);
		assert(outbound.front()
		           .envelope.avatar_rejected()
		           .authoritative_avatar()
		           .archetype_id()
		    == knight_soul);

		message->set_base_revision(2);
		avatar->set_revision(2);
		avatar->set_archetype_id(
		    "99999999-9999-4999-8999-999999999999");
		core.on_message(37, update, start + 6ms);
		outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.has_avatar_rejected());
		assert(outbound.front()
		           .envelope.avatar_rejected()
		           .authoritative_avatar()
		           .revision()
		    == 3);
		assert(outbound.front()
		           .envelope.avatar_rejected()
		           .authoritative_avatar()
		           .archetype_id()
		    == npc::default_soul_id);

		message->set_base_revision(3);
		avatar->set_revision(3);
		avatar->mutable_equipment(0)->set_definition_id(
		    "not-a-runtime-item-id");
		core.on_message(37, update, start + 2s);
		outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.has_avatar_rejected());
		assert(outbound.front().close_after_send == close_kind::none);
		assert(outbound.front()
		           .envelope.avatar_rejected()
		           .authoritative_avatar()
		           .revision()
		    == 3);
		assert(core.players().size() == 1);
	}

	{
		server_config config;
		config.default_avatar_archetype = "unknown.default";
		config.allowed_avatar_archetypes = {
		    "unknown.allowed",
		    std::string(npc::default_soul_id),
		    "unknown.allowed"};
		normalize_avatar_config(config);
		assert(config.default_avatar_archetype == npc::default_soul_id);
		assert(config.allowed_avatar_archetypes.size() == 1);
		assert(
		    config.allowed_avatar_archetypes.front()
		    == npc::default_soul_id);

		const std::string custom =
		    "11111111-2222-4333-8444-555555555555";
		config.known_avatar_archetypes.insert(custom);
		config.default_avatar_archetype = custom;
		config.allowed_avatar_archetypes = {custom};
		normalize_avatar_config(config);
		assert(config.allowed_avatar_archetypes.size() == 2);
		assert(std::ranges::find(
		           config.allowed_avatar_archetypes,
		           npc::default_soul_id)
		    != config.allowed_avatar_archetypes.end());
	}

	temporary_world authored_pickup_world;
	{
		server_core core(config_for(authored_pickup_world.path));
		protocol::PlayerProfile profile;
		(void)connect_new_player(core, 44, start, 1, &profile);
		(void)connect_new_player(core, 46, start + 1ms, 2, nullptr, "Hans");

		protocol::Envelope pickup;
		auto *update = pickup.mutable_client_world_item_update();
		update->set_base_revision(0);
		auto *state = update->mutable_state();
		state->set_instance_id(
		    "bbbbbbbb-0000-4000-8000-000000000001");
		state->set_revision(0);
		state->set_present(false);
		auto *item = state->mutable_item();
		item->set_instance_id(state->instance_id());
		item->set_definition_id(
		    "bbbbbbbb-0000-4000-8000-000000000002");
		item->set_count(1);
		item->set_quality(1.0F);
		item->set_condition(1.0F);
		state->mutable_transform()->mutable_position();
		state->mutable_transform()->mutable_rotation()->set_w(1.0F);
		state->mutable_transform()->mutable_velocity();
		core.on_message(44, pickup, start + 2ms);
		const auto outbound = core.take_outbound();
		const auto accepted = std::ranges::find_if(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.connection == 44
			        && message.envelope.has_world_item_accepted();
		    });
		assert(accepted != outbound.end());
		assert(accepted->envelope.world_item_accepted().revision() == 1);
		assert(accepted->envelope.world_item_accepted()
		           .has_authoritative_profile());
		assert(std::ranges::any_of(
		    accepted->envelope.world_item_accepted()
		        .authoritative_profile()
		        .inventory(),
		    [&](const protocol::InventoryItem &candidate)
		    { return candidate.instance_id() == state->instance_id(); }));
		assert(std::ranges::any_of(
		    outbound,
		    [&](const outbound_message &message)
		    {
			    return message.envelope.has_world_item_updated()
			        && message.envelope.world_item_updated().state().instance_id()
			            == state->instance_id()
			        && !message.envelope.world_item_updated().state().present();
		    }));
	}

	temporary_world native_acquisition_world;
	{
		server_core core(config_for(native_acquisition_world.path));
		protocol::PlayerProfile profile;
		(void)connect_new_player(core, 45, start, 1, &profile);

		protocol::Envelope acquired;
		auto *update = acquired.mutable_client_profile_update();
		update->set_base_revision(profile.revision());
		*update->mutable_profile() = profile;
		auto *item = update->mutable_profile()->add_inventory();
		item->set_instance_id(
		    "aaaaaaaa-0000-4000-8000-000000000001");
		item->set_definition_id(
		    "aaaaaaaa-0000-4000-8000-000000000002");
		item->set_count(1);
		item->set_quality(1.0F);
		item->set_condition(1.0F);
		core.on_message(45, acquired, start + 1ms);
		auto outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.has_profile_accepted());
		assert(outbound.front().envelope.profile_accepted().revision() == 2);

		profile = update->profile();
		profile.set_revision(2);
		protocol::Envelope stack_growth;
		auto *growth = stack_growth.mutable_client_profile_update();
		growth->set_base_revision(profile.revision());
		*growth->mutable_profile() = profile;
		growth->mutable_profile()->mutable_inventory(
		    growth->profile().inventory_size() - 1)->set_count(4);
		core.on_message(45, stack_growth, start + 2ms);
		outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.has_profile_accepted());
		assert(outbound.front().envelope.profile_accepted().revision() == 3);
	}

	temporary_world world_sync_world;
	std::string world_identity_token;
	{
		server_core core(config_for(world_sync_world.path));
		protocol::PlayerProfile first_profile;
		protocol::PlayerProfile second_profile;
		world_identity_token = connect_new_player(
		    core,
		    50,
		    start,
		    1,
		    &first_profile);
		(void)connect_new_player(
		    core,
		    51,
		    start + 4ms,
		    2,
		    &second_profile,
		    "Hans");

		protocol::Envelope fabricated_item;
		auto *fabricated_update =
		    fabricated_item.mutable_client_profile_update();
		fabricated_update->set_base_revision(first_profile.revision());
		*fabricated_update->mutable_profile() = first_profile;
		// A known UUID owned by another player remains protected even though
		// native game-origin acquisitions are accepted below.
		*fabricated_update->mutable_profile()->add_inventory() =
		    second_profile.inventory(0);
		core.on_message(50, fabricated_item, start + 7ms);
		auto outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.has_profile_rejected());
		assert(
		    outbound.front().envelope.profile_rejected().reason()
		    == "item is already owned by another player");

		protocol::Envelope observed_container;
		auto *container_update =
		    observed_container.mutable_client_world_object_update();
		container_update->set_base_revision(0);
		auto *container = container_update->mutable_state();
		container->set_entity_guid(0x12345678ULL);
		container->set_kind(protocol::WORLD_OBJECT_KIND_CONTAINER);
		container->set_revision(0);
		container->set_opened(true);
		container->set_has_inventory(true);
		auto *loot = container->add_inventory();
		loot->set_instance_id(
		    "cccccccc-cccc-4ccc-8ccc-cccccccccccc");
		loot->set_definition_id(
		    "dddddddd-dddd-4ddd-8ddd-dddddddddddd");
		loot->set_count(1);
		loot->set_quality(100.0F);
		loot->set_condition(1.0F);
		core.on_message(50, observed_container, start + 8ms);
		outbound = core.take_outbound();
		assert(std::ranges::any_of(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.connection == 50
			        && message.envelope.has_world_object_accepted()
			        && message.envelope.world_object_accepted().revision() == 1;
		    }));
		assert(std::ranges::any_of(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.connection == 51
			        && message.envelope.has_world_object_updated()
			        && message.envelope.world_object_updated()
			               .state()
			               .inventory_size()
			            == 1;
		    }));

		protocol::Envelope first_loot_update;
		auto *first_profile_update =
		    first_loot_update.mutable_client_profile_update();
		first_profile_update->set_base_revision(first_profile.revision());
		*first_profile_update->mutable_profile() = first_profile;
		*first_profile_update->mutable_profile()->add_inventory() = *loot;
		core.on_message(50, first_loot_update, start + 9ms);
		outbound = core.take_outbound();
		assert(std::ranges::any_of(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.connection == 50
			        && message.envelope.has_profile_accepted()
			        && message.envelope.profile_accepted().revision() == 2;
		    }));
		assert(std::ranges::any_of(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.envelope.has_world_object_updated()
			        && message.envelope.world_object_updated().state().revision()
			            == 2
			        && message.envelope.world_object_updated()
			               .state()
			               .inventory_size()
			            == 0;
		    }));

		protocol::Envelope duplicate_loot_update;
		auto *second_profile_update =
		    duplicate_loot_update.mutable_client_profile_update();
		second_profile_update->set_base_revision(second_profile.revision());
		*second_profile_update->mutable_profile() = second_profile;
		*second_profile_update->mutable_profile()->add_inventory() = *loot;
		core.on_message(51, duplicate_loot_update, start + 10ms);
		outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.has_profile_rejected());
		assert(outbound.front().close_after_send == close_kind::none);
		assert(outbound.front()
		           .envelope.profile_rejected()
		           .authoritative_profile()
		           .revision()
		    == second_profile.revision());

		auto stale_container = observed_container;
		stale_container.mutable_client_world_object_update()
		    ->set_base_revision(1);
		stale_container.mutable_client_world_object_update()
		    ->mutable_state()
		    ->set_revision(1);
		stale_container.mutable_client_world_object_update()
		    ->mutable_state()
		    ->set_opened(false);
		core.on_message(51, stale_container, start + 11ms);
		outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.has_world_object_rejected());
		const auto &authoritative = outbound.front()
		                                .envelope.world_object_rejected()
		                                .authoritative_state();
		assert(authoritative.revision() == 2);
		assert(authoritative.inventory_size() == 0);

		auto reintroduced_loot = observed_container;
		reintroduced_loot.mutable_client_world_object_update()
		    ->set_base_revision(2);
		reintroduced_loot.mutable_client_world_object_update()
		    ->mutable_state()
		    ->set_revision(2);
		core.on_message(51, reintroduced_loot, start + 12ms);
		outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.has_world_object_rejected());
		assert(outbound.front()
		           .envelope.world_object_rejected()
		           .authoritative_state()
		           .revision()
		    == 2);
		assert(outbound.front()
		           .envelope.world_object_rejected()
		           .authoritative_state()
		           .inventory_size()
		    == 0);

		protocol::Envelope dropped_item;
		auto *drop_update = dropped_item.mutable_client_world_item_update();
		drop_update->set_base_revision(0);
		auto *drop = drop_update->mutable_state();
		drop->set_instance_id(loot->instance_id());
		drop->set_revision(0);
		drop->set_present(true);
		*drop->mutable_item() = *loot;
		auto *drop_transform = drop->mutable_transform();
		drop_transform->mutable_position()->set_x(1.0F);
		drop_transform->mutable_position()->set_y(2.0F);
		drop_transform->mutable_position()->set_z(3.0F);
		drop_transform->mutable_rotation()->set_w(1.0F);
		drop_transform->mutable_velocity();

		auto mutated_drop = dropped_item;
		mutated_drop.mutable_client_world_item_update()
		    ->mutable_state()
		    ->mutable_item()
		    ->set_definition_id(
		        "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
		core.on_message(50, mutated_drop, start + 13ms);
		outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.has_world_item_rejected());
		assert(!outbound.front()
		            .envelope.world_item_rejected()
		            .authoritative_state()
		            .present());

		core.on_message(50, dropped_item, start + 13ms);
		outbound = core.take_outbound();
		assert(std::ranges::any_of(
		    outbound,
		    [](const outbound_message &message)
			{
				return message.connection == 50
				    && message.envelope.has_world_item_accepted()
				    && message.envelope.world_item_accepted().revision() == 1
				    && message.envelope.world_item_accepted()
				           .has_authoritative_profile()
				    && message.envelope.world_item_accepted()
				           .authoritative_profile()
				           .revision()
				        == 3
				    && std::ranges::none_of(
				        message.envelope.world_item_accepted()
				            .authoritative_profile()
				            .inventory(),
				        [](const protocol::InventoryItem &item)
				        {
					        return item.instance_id()
					            == "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
				        });
			}));
		assert(std::ranges::any_of(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.connection == 51
			        && message.envelope.has_world_item_updated()
			        && message.envelope.world_item_updated().state().present();
		    }));

		protocol::Envelope picked_up_item;
		auto *pickup_profile =
		    picked_up_item.mutable_client_profile_update();
		pickup_profile->set_base_revision(second_profile.revision());
		*pickup_profile->mutable_profile() = second_profile;
		*pickup_profile->mutable_profile()->add_inventory() = *loot;
		core.on_message(51, picked_up_item, start + 14ms);
		outbound = core.take_outbound();
		assert(std::ranges::any_of(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.connection == 51
			        && message.envelope.has_profile_accepted();
		    }));
		assert(std::ranges::any_of(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.envelope.has_world_item_updated()
			        && !message.envelope.world_item_updated().state().present()
			        && message.envelope.world_item_updated().state().revision() == 2;
		    }));
	}
	{
		server_core restarted(config_for(world_sync_world.path));
		restarted.on_transport_connected(52, start + 1s);
		restarted.on_message(52, hello(), start + 1s);
		(void)restarted.take_outbound();
		restarted.on_message(
		    52,
		    authenticate(world_identity_token),
		    start + 1s + 1ms);
		const auto bootstrap = find_bootstrap(restarted.take_outbound(), 52);
		assert(bootstrap.world_objects_size() == 0);
		restarted.on_message(52, ready(bootstrap), start + 1s + 2ms);
		const auto outbound = restarted.take_outbound();
		assert(std::ranges::any_of(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.connection == 52
			        && message.envelope.has_world_object_updated()
			        && message.envelope.world_object_updated()
			               .state()
			               .entity_guid()
			            == 0x12345678ULL
			        && message.envelope.world_object_updated().state().revision()
			            == 2
			        && message.envelope.world_object_updated()
			               .state()
			               .inventory_size()
			            == 0;
		    }));
		assert(std::ranges::any_of(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.connection == 52
			        && message.envelope.has_world_item_updated()
			        && !message.envelope.world_item_updated().state().present()
			        && message.envelope.world_item_updated().state().revision() == 2;
		    }));
	}

	temporary_world split_transfer_world;
	{
		auto config = config_for(split_transfer_world.path);
		config.starter_profile.inventory.push_back({
		    .definition_id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
		    .count = 10,
		    .quality = 1.0F,
		    .condition = 0.8F});
		server_core core(config);
		protocol::PlayerProfile first_profile;
		protocol::PlayerProfile second_profile;
		(void)connect_new_player(core, 53, start, 1, &first_profile);
		(void)connect_new_player(
		    core, 54, start + 1ms, 2, &second_profile, "Hans");
		const auto source_item = std::ranges::find_if(
		    first_profile.inventory(),
		    [](const protocol::InventoryItem &item)
		    {
			    return item.definition_id()
			        == "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
		    });
		assert(source_item != first_profile.inventory().end());
		const auto source_instance = source_item->instance_id();

		protocol::Envelope local_split;
		auto *local_split_update =
		    local_split.mutable_client_profile_update();
		local_split_update->set_base_revision(first_profile.revision());
		*local_split_update->mutable_profile() = first_profile;
		for (auto &item :
		     *local_split_update->mutable_profile()->mutable_inventory())
		{
			if (item.instance_id() == source_instance)
				item.set_count(7);
		}
		auto *local_split_item =
		    local_split_update->mutable_profile()->add_inventory();
		*local_split_item = *source_item;
		local_split_item->set_instance_id(
		    "66666666-6666-4666-8666-666666666666");
		local_split_item->set_count(3);
		local_split_item->clear_equipped_slot();
		core.on_message(53, local_split, start + 2ms);
		auto outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.has_profile_accepted());
		assert(outbound.front().envelope.profile_accepted().revision() == 2);

		auto merged_profile = local_split_update->profile();
		merged_profile.set_revision(2);
		for (auto &item : *merged_profile.mutable_inventory())
		{
			if (item.instance_id() == source_instance)
				item.set_count(10);
		}
		for (auto index = merged_profile.inventory_size(); index-- > 0;)
		{
			if (merged_profile.inventory(index).instance_id()
			    == "66666666-6666-4666-8666-666666666666")
			{
				merged_profile.mutable_inventory()->DeleteSubrange(index, 1);
			}
		}
		protocol::Envelope local_merge;
		auto *local_merge_update =
		    local_merge.mutable_client_profile_update();
		local_merge_update->set_base_revision(2);
		*local_merge_update->mutable_profile() = merged_profile;
		core.on_message(53, local_merge, start + 3ms);
		outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.has_profile_accepted());
		assert(outbound.front().envelope.profile_accepted().revision() == 3);
		first_profile = std::move(merged_profile);
		const auto merged_source = std::ranges::find_if(
		    first_profile.inventory(),
		    [&](const protocol::InventoryItem &item)
		    { return item.instance_id() == source_instance; });
		assert(merged_source != first_profile.inventory().end());

		protocol::Envelope split_drop;
		auto *split_update = split_drop.mutable_client_world_item_update();
		split_update->set_base_revision(0);
		split_update->set_source_instance_id(
		    merged_source->instance_id());
		split_update->set_transfer_count(3);
		auto *world_item = split_update->mutable_state();
		world_item->set_instance_id(
		    "55555555-5555-4555-8555-555555555555");
		world_item->set_revision(0);
		world_item->set_present(true);
		*world_item->mutable_item() = *merged_source;
		world_item->mutable_item()->set_instance_id(world_item->instance_id());
		world_item->mutable_item()->set_count(3);
		world_item->mutable_item()->clear_equipped_slot();
		world_item->mutable_transform()->mutable_position();
		world_item->mutable_transform()->mutable_rotation()->set_w(1.0F);
		world_item->mutable_transform()->mutable_velocity();
		core.on_message(53, split_drop, start + 4ms);
		outbound = core.take_outbound();
		const auto accepted = std::ranges::find_if(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.connection == 53
			        && message.envelope.has_world_item_accepted();
		    });
		assert(accepted != outbound.end());
		assert(accepted->envelope.world_item_accepted().revision() == 1);
		assert(std::ranges::any_of(
		    accepted->envelope.world_item_accepted()
		        .authoritative_profile()
		        .inventory(),
		    [](const protocol::InventoryItem &item)
		    {
			    return item.definition_id()
			            == "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
			        && item.count() == 7;
		    }));
		const auto first_after_split =
		    accepted->envelope.world_item_accepted().authoritative_profile();

		protocol::Envelope pickup;
		auto *pickup_update = pickup.mutable_client_world_item_update();
		pickup_update->set_base_revision(1);
		*pickup_update->mutable_state() = *world_item;
		pickup_update->mutable_state()->set_revision(1);
		pickup_update->mutable_state()->set_present(false);
		core.on_message(54, pickup, start + 5ms);
		outbound = core.take_outbound();
		const auto pickup_accepted = std::ranges::find_if(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.connection == 54
			        && message.envelope.has_world_item_accepted();
		    });
		assert(pickup_accepted != outbound.end());
		assert(pickup_accepted->envelope.world_item_accepted().revision() == 2);
		assert(std::ranges::any_of(
		    pickup_accepted->envelope.world_item_accepted()
		        .authoritative_profile()
		        .inventory(),
		    [&](const protocol::InventoryItem &item)
		    { return item.instance_id() == world_item->instance_id(); }));

		protocol::Envelope duplicate_claim;
		auto *claim_update = duplicate_claim.mutable_client_profile_update();
		claim_update->set_base_revision(first_after_split.revision());
		*claim_update->mutable_profile() = first_after_split;
		*claim_update->mutable_profile()->add_inventory() = world_item->item();
		core.on_message(53, duplicate_claim, start + 6ms);
		outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().envelope.has_profile_rejected());
		assert(
		    outbound.front().envelope.profile_rejected().reason()
		    == "item is already owned by another player");
	}

	temporary_world lease_world;
	{
		auto config = config_for(lease_world.path, false);
		config.bootstrap_timeout_seconds = 30;
		server_core core(config);

		core.on_transport_connected(40, start);
		core.on_message(40, hello("Henry"), start);
		(void)core.take_outbound();
		core.on_message(40, enroll(), start + 1ms);
		assert(find_bootstrap(core.take_outbound(), 40).mode()
		    == protocol::BOOTSTRAP_MODE_INITIALIZE);

		core.on_transport_connected(41, start + 2ms);
		core.on_message(41, hello("Hans"), start + 2ms);
		(void)core.take_outbound();
		core.on_message(41, enroll(), start + 3ms);
		assert(find_bootstrap(core.take_outbound(), 41).mode()
		    == protocol::BOOTSTRAP_MODE_WAIT);

		core.on_transport_disconnected(
		    40,
		    false,
		    "initializer disconnected",
		    start + 4ms);
		assert(find_bootstrap(core.take_outbound(), 41).mode()
		    == protocol::BOOTSTRAP_MODE_INITIALIZE);

		core.tick(start + 31s);
		assert(core.take_outbound().empty());
		assert(core.pending_connection_count() == 1);

		core.tick(start + 30min);
		assert(core.take_outbound().empty());
		assert(core.pending_connection_count() == 1);

		core.on_transport_disconnected(
		    41,
		    false,
		    "loader disconnected",
		    start + 30min + 1ms);
		assert(core.pending_connection_count() == 0);

		core.on_transport_connected(42, start + 30min + 2ms);
		core.tick(start + 30min + 12s);
		assert(has_rejection(
		    core.take_outbound(),
		    42,
		    protocol::REJECT_REASON_BOOTSTRAP_FAILED));
	}

	temporary_world activity_world;
	{
		server_core core(config_for(activity_world.path));
		(void)connect_new_player(core, 60, start, 1, nullptr, "Henry");
		(void)connect_new_player(core, 61, start + 10ms, 2, nullptr, "Hans");

		protocol::Envelope first_start;
		auto *request = first_start.mutable_client_activity_start();
		request->set_kind(protocol::PLAYER_ACTIVITY_KIND_BLACKSMITHING);
		request->set_station_guid(0xAABBCCDDULL);
		core.on_message(60, first_start, start + 20ms);
		auto outbound = core.take_outbound();
		const auto granted = std::ranges::find_if(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.connection == 60
			        && message.envelope.has_activity_granted();
		    });
		assert(granted != outbound.end());
		const auto session_id =
		    granted->envelope.activity_granted().activity().session_id();
		assert(session_id != 0);
		assert(std::ranges::any_of(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.connection == 61
			        && message.envelope.has_player_activity_updated()
			        && message.envelope.player_activity_updated()
			               .activity()
			               .active();
		    }));

		core.on_message(61, first_start, start + 21ms);
		outbound = core.take_outbound();
		assert(outbound.size() == 1);
		assert(outbound.front().connection == 61);
		assert(outbound.front().envelope.has_activity_denied());

		protocol::Envelope first_end;
		first_end.mutable_client_activity_end()->set_session_id(session_id);
		core.on_message(60, first_end, start + 22ms);
		outbound = core.take_outbound();
		assert(std::ranges::count_if(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.envelope.has_player_activity_updated()
			        && !message.envelope.player_activity_updated()
			                .activity()
			                .active();
		    }) == 2);

		core.on_message(61, first_start, start + 23ms);
		outbound = core.take_outbound();
		assert(std::ranges::any_of(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.connection == 61
			        && message.envelope.has_activity_granted();
		    }));
		core.on_transport_disconnected(
		    61,
		    true,
		    "temporary disconnect",
		    start + 24ms);
		outbound = core.take_outbound();
		assert(std::ranges::any_of(
		    outbound,
		    [](const outbound_message &message)
		    {
			    return message.connection == 60
			        && message.envelope.has_player_activity_updated()
			        && !message.envelope.player_activity_updated()
			                .activity()
			                .active();
		    }));
	}

	return 0;
}
