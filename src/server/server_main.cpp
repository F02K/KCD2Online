#include "multiplayer/networking.hpp"
#include "multiplayer/protocol.hpp"
#include "server/server_config.hpp"
#include "server/server_core.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace
{
	struct command_queue
	{
		std::mutex mutex;
		std::deque<std::string> commands;
	};

	void print_help()
	{
		std::cout
		    << "Commands: status, players, kick <player_id> [reason], "
		       "say <text>, profile claim <player_id>, "
		       "dummy spawn [name], dummy remove <player_id>, "
		       "entities <all|humans|animals> <disable|enable>, "
		       "entities status, time <hours>, timescale <ratio>, "
		       "weather <id> [transition_seconds], stop, help\n";
		std::cout
		    << "Property: property list [filter], property show <property_id>, "
		       "property owner <property_id> <player_id>, property grant "
		       "<actor_player_id> <property_id> <target_player_id> "
		       "<steward|resident|employee|guard|guest> [minutes], property "
		       "revoke <actor_player_id|system> <assignment_id>\n";
	}

	std::optional<kcd2o::protocol::PropertyRole> property_role(
	    std::string_view value)
	{
		using namespace kcd2o::protocol;
		if (value == "steward")
			return PROPERTY_ROLE_STEWARD;
		if (value == "resident")
			return PROPERTY_ROLE_RESIDENT;
		if (value == "employee")
			return PROPERTY_ROLE_EMPLOYEE;
		if (value == "guard")
			return PROPERTY_ROLE_GUARD;
		if (value == "guest")
			return PROPERTY_ROLE_GUEST;
		return std::nullopt;
	}

	int close_reason(kcd2o::server::close_kind kind)
	{
		using kcd2o::server::close_kind;
		switch (kind)
		{
		case close_kind::reject:
			return kcd2o::net::server_rejected_reason;
		case close_kind::shutdown:
			return kcd2o::net::server_shutdown_reason;
		case close_kind::kick:
			return kcd2o::net::server_kicked_reason;
		case close_kind::none:
			break;
		}
		return 0;
	}
}

int main(int argc, char **argv)
{
	using namespace std::chrono_literals;
	try
	{
		const auto config_path = argc > 1
		    ? std::filesystem::path(argv[1])
		    : std::filesystem::path("server.toml");
		const auto config = kcd2o::server::load_server_config(config_path);
		kcd2o::net::runtime network_runtime;
		kcd2o::server::server_core core(config);

		auto console = std::make_shared<command_queue>();
		std::thread(
		    [console]
		    {
			    std::string line;
			    while (std::getline(std::cin, line))
			    {
				    if (line.starts_with("\xEF\xBB\xBF"))
				    {
					    line.erase(0, 3);
				    }
				    std::scoped_lock lock(console->mutex);
				    console->commands.push_back(std::move(line));
			    }
		    })
		    .detach();

		const auto now = []
		{
			return kcd2o::server::clock::now();
		};

		kcd2o::net::server_transport transport({
		    .connected =
		        [&](kcd2o::connection_id connection)
		        {
			        std::cout << "connection " << connection << " accepted from "
			                  << transport.connection_description(connection) << '\n';
			        core.on_transport_connected(connection, now());
		        },
		    .disconnected =
		        [&](kcd2o::connection_id connection,
		            bool allow_reconnect,
		            std::string reason)
		        {
			        std::cout << "connection " << connection << " closed: "
			                  << reason << '\n';
			        core.on_transport_disconnected(
			            connection,
			            allow_reconnect,
			            std::move(reason),
			            now());
		        },
		    .message =
		        [&](kcd2o::connection_id connection,
		            std::span<const std::byte> bytes)
		        {
			        std::string error;
			        const auto envelope = kcd2o::decode(bytes, &error);
			        if (!envelope)
			        {
				        std::cerr << "connection " << connection
				                  << " sent malformed data: " << error << '\n';
				        transport.close(
				            connection,
				            kcd2o::net::server_rejected_reason,
				            "malformed message",
				            false);
				        return;
			        }
			        core.on_message(connection, *envelope, now());
		        }});

		transport.listen(config.bind_address, config.port);
		std::cout << config.name << " (KCD2Online " << kcd2o::kcd2o_version
		          << ", prototype) listening on " << config.bind_address << ':'
		          << config.port << " for level " << config.level_id << '\n';
		print_help();

		bool running = true;
		const auto tick_duration =
		    std::chrono::duration<double>(1.0 / config.tick_rate);
		auto next_tick = kcd2o::server::clock::now();
		while (running)
		{
			transport.poll();

			std::deque<std::string> commands;
			{
				std::scoped_lock lock(console->mutex);
				commands.swap(console->commands);
			}
			for (const auto &line : commands)
			{
				std::istringstream input(line);
				std::string command;
				input >> command;
				if (command == "status")
				{
					std::cout << "players=" << core.players().size()
					          << '/' << config.max_players
					          << " pending=" << core.pending_connection_count()
					          << " tick=" << core.server_tick()
					          << " human_npcs="
					          << (core.human_npcs_disabled()
					                  ? "disabled"
					                  : "enabled")
					          << " animal_npcs="
					          << (core.animal_npcs_disabled()
					                  ? "disabled"
					                  : "enabled")
					          << '\n';
					const auto environment = core.current_environment(now());
					std::cout << "time=" << environment.time_of_day_hours()
					          << " scale=" << environment.time_scale()
					          << " weather=" << environment.weather_id()
					          << " env_revision=" << environment.revision()
					          << '\n';
				}
				else if (command == "players")
				{
					for (const auto &player : core.players())
					{
						std::cout << player.id << "  " << player.persistent_id
						          << "  " << player.display_name
						          << "  "
						          << (player.dummy
						                  ? "dummy"
						                  : (player.connected
						                         ? "connected"
						                         : "reconnecting"))
						          << '\n';
					}
				}
				else if (command == "property")
				{
					std::string action;
					input >> action;
					if (action == "list")
					{
						std::string filter;
						std::getline(input >> std::ws, filter);
						for (const auto &property : core.property_catalog().properties())
						{
							if (!filter.empty()
							    && !property.property_id().contains(filter)
							    && !property.inferred_name().contains(filter)
							    && !property.source_path().contains(filter))
								continue;
							std::cout << property.property_id() << "  "
							          << property.inferred_name() << "  resources="
							          << property.resources_size() << " confidence="
							          << property.discovery_confidence() << "  "
							          << property.source_path();
							if (property.has_marker_position())
								std::cout << "  home=("
								          << property.marker_position().x() << ','
								          << property.marker_position().y() << ','
								          << property.marker_position().z() << ')';
							std::cout << '\n';
						}
					}
					else if (action == "show")
					{
						std::string property_id;
						input >> property_id;
						const auto found = std::ranges::find_if(
						    core.property_catalog().properties(),
						    [&](const auto &property)
						    { return property.property_id() == property_id; });
						if (found == core.property_catalog().properties().end())
						{
							std::cout << "unknown property\n";
							continue;
						}
						std::cout << found->property_id() << "  "
						          << found->inferred_name() << "  "
						          << found->source_path() << '\n';
						for (const auto &assignment :
						     core.property_ledger().assignments())
						{
							if (assignment.property_id() == property_id)
								std::cout << "  " << assignment.assignment_id()
								          << " player="
								          << assignment.subject_player_id()
								          << " role=" << assignment.role()
								          << " expires="
								          << assignment.expires_at_ms() << '\n';
						}
					}
					else if (action == "owner")
					{
						std::string property_id;
						kcd2o::player_id target{};
						input >> property_id >> target;
						std::string error;
						if (property_id.empty() || target == 0
						    || !core.assign_property_owner(
						        property_id, target, error))
							std::cout << "could not assign owner: " << error << '\n';
						else
							std::cout << "property owner assigned\n";
					}
					else if (action == "grant")
					{
						kcd2o::player_id actor{};
						kcd2o::player_id target{};
						std::string property_id;
						std::string role_name;
						std::uint64_t minutes{};
						input >> actor >> property_id >> target >> role_name;
						input >> minutes;
						const auto role = property_role(role_name);
						const auto expires = minutes == 0 ? 0ULL
						    : static_cast<std::uint64_t>(
						          std::chrono::duration_cast<std::chrono::milliseconds>(
						              std::chrono::system_clock::now().time_since_epoch())
						              .count())
						        + minutes * 60'000ULL;
						std::string error;
						if (!role || actor == 0 || target == 0
						    || !core.grant_property_role(
						        actor,
						        property_id,
						        target,
						        *role,
						        expires,
						        error))
							std::cout << "could not grant role: " << error << '\n';
						else
							std::cout << "property role granted\n";
					}
					else if (action == "revoke")
					{
						std::string actor;
						std::string assignment;
						input >> actor >> assignment;
						std::string error;
						bool revoked = false;
						if (actor == "system")
							revoked = core.system_revoke_property_role(
							    assignment, error);
						else
						{
							std::istringstream actor_input(actor);
							kcd2o::player_id actor_id{};
							if (actor_input >> actor_id)
								revoked = core.revoke_property_role(
								    actor_id, assignment, error);
						}
						if (!revoked)
							std::cout << "could not revoke role: " << error << '\n';
						else
							std::cout << "property role revoked\n";
					}
					else
					{
						std::cout << "usage: property <list|show|owner|grant|revoke>\n";
					}
				}
				else if (command == "kick")
				{
					kcd2o::player_id id{};
					input >> id;
					std::string reason;
					std::getline(input >> std::ws, reason);
					core.kick(
					    id,
					    reason.empty() ? "kicked by server" : reason,
					    now());
				}
				else if (command == "say")
				{
					std::string text;
					std::getline(input >> std::ws, text);
					core.server_say(std::move(text), now());
				}
				else if (command == "dummy")
				{
					std::string action;
					input >> action;
					if (action == "spawn")
					{
						std::string name;
						std::getline(input >> std::ws, name);
						std::string error;
						if (const auto id =
						        core.spawn_dummy(std::move(name), &error))
						{
							std::cout << "spawned dummy player " << *id
							          << '\n';
						}
						else
						{
							std::cout << "could not spawn dummy: " << error
							          << '\n';
						}
					}
					else if (action == "remove")
					{
						kcd2o::player_id id{};
						input >> id;
						if (id == 0 || !core.remove_dummy(id, now()))
						{
							std::cout << "unknown dummy player\n";
						}
						else
						{
							std::cout << "removed dummy player " << id
							          << '\n';
						}
					}
					else
					{
						std::cout
						    << "usage: dummy <spawn [name]|remove <player_id>>\n";
					}
				}
				else if (command == "profile")
				{
					std::string action;
					kcd2o::player_id id{};
					input >> action >> id;
					if (action != "claim" || id == 0)
					{
						std::cout << "usage: profile claim <player_id>\n";
					}
					else if (const auto code =
					             core.create_profile_claim(id, now()))
					{
						std::cout << "claim code for " << id << ": " << *code
						          << " (valid for 10 minutes)\n";
					}
					else
					{
						std::cout << "unknown player profile\n";
					}
				}
				else if (command == "entities")
				{
					std::string target;
					input >> target;
					if (target == "status")
					{
						std::cout << "human NPCs are "
						          << (core.human_npcs_disabled()
						                  ? "disabled"
						                  : "enabled")
						          << "; animal NPCs are "
						          << (core.animal_npcs_disabled()
						                  ? "disabled"
						                  : "enabled")
						          << '\n';
					}
					else
					{
						std::string action;
						if (target == "disable" || target == "enable")
						{
							action = target;
							target = "all";
						}
						else
						{
							input >> action;
						}
						if ((target != "all" && target != "humans"
						        && target != "animals")
						    || (action != "disable" && action != "enable"))
						{
							std::cout << "usage: entities "
							             "<all|humans|animals> <disable|enable> "
							             "or entities status\n";
							continue;
						}
						const bool disabled = action == "disable";
						auto humans = core.human_npcs_disabled();
						auto animals = core.animal_npcs_disabled();
						if (target == "all" || target == "humans")
							humans = disabled;
						if (target == "all" || target == "animals")
							animals = disabled;
						const bool changed = core.set_npc_entities_disabled(
						    humans,
						    animals);
						std::cout << target << " NPCs "
						          << (disabled ? "disabled" : "enabled")
						          << (changed ? "" : " (unchanged)") << '\n';
					}
				}
				else if (command == "time")
				{
					double hours{};
					if (!(input >> hours) || hours < 0.0 || hours >= 24.0)
					{
						std::cout << "usage: time <hours 0..23.999>\n";
					}
					else
					{
						const bool changed = core.set_time_of_day(hours, now());
						std::cout << "time set to " << hours
						          << (changed ? "" : " (unchanged)") << '\n';
					}
				}
				else if (command == "timescale")
				{
					float scale{};
					if (!(input >> scale) || scale < 0.0F || scale > 1000.0F)
					{
						std::cout << "usage: timescale <ratio 0..1000>\n";
					}
					else
					{
						const bool changed = core.set_time_scale(scale, now());
						std::cout << "time scale set to " << scale
						          << (changed ? "" : " (unchanged)") << '\n';
					}
				}
				else if (command == "weather")
				{
					std::uint32_t id{};
					std::uint32_t transition = 30;
					if (!(input >> id))
					{
						std::cout << "usage: weather <id 1..33> "
						             "[transition_seconds 0..600]\n";
						continue;
					}
					if (input >> transition)
					{
						// Optional transition parsed successfully.
					}
					if (id < 1 || id > 33 || transition > 600)
					{
						std::cout << "usage: weather <id 1..33> "
						             "[transition_seconds 0..600]\n";
					}
					else
					{
						const bool changed = core.set_weather(id, transition, now());
						std::cout << "weather set to " << id << " over "
						          << transition << " seconds"
						          << (changed ? "" : " (unchanged)") << '\n';
					}
				}
				else if (command == "stop")
				{
					core.shutdown("server stopped");
					running = false;
				}
				else if (command == "help")
				{
					print_help();
				}
				else if (!command.empty())
				{
					std::cerr << "unknown command: " << command << '\n';
				}
			}

			const auto tick_now = now();
			if (tick_now >= next_tick)
			{
				core.tick(tick_now);
				next_tick = tick_now
				    + std::chrono::duration_cast<kcd2o::server::clock::duration>(
				        tick_duration);
			}

			for (auto &outbound : core.take_outbound())
			{
				std::string error;
				const auto encoded =
				    kcd2o::encode(outbound.envelope, outbound.delivery, &error);
				bool sent = false;
				bool congested = false;
				if (encoded)
				{
					const auto lane = kcd2o::lane_for(outbound.envelope);
					constexpr std::size_t unreliable_queue_limit = 96 * 1024;
					const auto pending =
					    transport.pending_send_bytes(outbound.connection, lane);
					congested = outbound.delivery
					        == kcd2o::reliability::unreliable
					    && pending && *pending >= unreliable_queue_limit;
					if (!congested)
						sent = transport.send(
						    outbound.connection,
						    encoded->bytes,
						    outbound.delivery,
						    lane,
						    &error,
						    &congested);
				}
				if (!encoded)
					std::cerr << "encode for connection " << outbound.connection
					          << " failed: " << error << '\n';
				else if (!sent
				    && !(congested
				        && outbound.delivery
				            == kcd2o::reliability::unreliable))
				{
					std::cerr << "send to connection " << outbound.connection
					          << " failed: " << error << '\n';
					if (outbound.delivery == kcd2o::reliability::reliable)
						transport.close(
						    outbound.connection,
						    kcd2o::net::server_kicked_reason,
						    "reliable send failed",
						    false);
				}
				if (outbound.close_after_send
				    != kcd2o::server::close_kind::none)
				{
					transport.close(
					    outbound.connection,
					    close_reason(outbound.close_after_send),
					    "connection closed by server",
					    true);
				}
			}

			std::this_thread::sleep_for(1ms);
		}
		return 0;
	}
	catch (const std::exception &exception)
	{
		std::cerr << "KCD2OnlineServer fatal error: " << exception.what() << '\n';
		return 1;
	}
}
