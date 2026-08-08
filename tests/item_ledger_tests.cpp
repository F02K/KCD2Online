#include "server/item_ledger.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
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
	using namespace kcd2o;
	using namespace kcd2o::server;

	protocol::InventoryItem item(
	    std::string instance,
	    std::uint32_t count,
	    float quality = 50.0F)
	{
		protocol::InventoryItem result;
		result.set_instance_id(std::move(instance));
		result.set_definition_id(
		    "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
		result.set_count(count);
		result.set_quality(quality);
		result.set_condition(0.75F);
		return result;
	}
}

int main()
{
	item_ledger ledger;
	std::string error;
	const auto source_id = "11111111-1111-4111-8111-111111111111";
	const auto split_id = "22222222-2222-4222-8222-222222222222";
	const auto merge_id = "33333333-3333-4333-8333-333333333333";
	const auto mismatch_id = "44444444-4444-4444-8444-444444444444";
	const auto player = item_location::player(7);
	const auto container = item_location::container(99);
	const auto world = item_location::world();

	assert(ledger.register_item(item(source_id, 10), player, error));
	assert(!ledger.register_item(item(source_id, 10), container, error));
	assert(ledger.size() == 1);

	error.clear();
	assert(ledger.split(source_id, player, split_id, 4, world, error));
	assert(ledger.find(source_id)->item.count() == 6);
	assert(ledger.find(split_id)->item.count() == 4);
	assert(ledger.find(split_id)->location == world);
	assert(!ledger.split(source_id, player, split_id, 2, world, error));
	assert(ledger.find(source_id)->item.count() == 6);

	error.clear();
	assert(ledger.move(split_id, world, container, error));
	assert(ledger.register_item(item(merge_id, 2), container, error));
	assert(ledger.merge(split_id, container, merge_id, container, 4, error));
	assert(ledger.find(split_id) == nullptr);
	assert(ledger.find(merge_id)->item.count() == 6);

	assert(ledger.register_item(item(mismatch_id, 1, 25.0F), container, error));
	assert(!ledger.merge(
	    mismatch_id, container, merge_id, container, 1, error));
	assert(ledger.find(mismatch_id)->item.count() == 1);
	assert(ledger.find(merge_id)->item.count() == 6);

	auto changed_definition = ledger.find(source_id)->item;
	changed_definition.set_definition_id(
	    "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
	assert(!ledger.replace_item(changed_definition, player, error));
	assert(
	    ledger.find(source_id)->item.definition_id()
	    == "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");

	std::cout << "item ledger tests passed\n";
	return 0;
}
