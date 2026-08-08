#include "multiplayer/game_command_queue.hpp"

#include <cassert>

int main()
{
	kcd2o::game_command_queue queue(2);
	kcd2o::protocol::Envelope snapshot_one;
	snapshot_one.mutable_world_snapshot()->set_server_tick(1);
	assert(queue.push(std::move(snapshot_one), false));

	kcd2o::protocol::Envelope snapshot_two;
	snapshot_two.mutable_world_snapshot()->set_server_tick(2);
	assert(queue.push(std::move(snapshot_two), false));
	assert(queue.size() == 1);

	kcd2o::protocol::Envelope chat;
	chat.mutable_chat_broadcast()->set_text("hello");
	assert(queue.push(std::move(chat), true));
	assert(queue.size() == 2);

	kcd2o::protocol::Envelope reliable;
	reliable.mutable_player_left()->set_player_id(4);
	assert(queue.push(std::move(reliable), true));
	assert(queue.size() == 2);

	const auto drained = queue.drain();
	assert(drained.size() == 2);
	assert(drained.front().has_chat_broadcast());
	assert(drained.back().has_player_left());

	kcd2o::protocol::Envelope stale;
	stale.mutable_player_left()->set_player_id(9);
	assert(queue.push(std::move(stale), true));
	queue.clear();
	assert(queue.size() == 0);
	return 0;
}
