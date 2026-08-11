#include "kcse/client_api.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace
{
	std::uint32_t __cdecl runtime(
	    kcd2o::kcse::runtime_status *) noexcept
	{
		return 1;
	}
	std::uint32_t __cdecl connect(
	    const kcd2o::kcse::connect_request *) noexcept
	{
		return 1;
	}
	void __cdecl disconnect() noexcept {}
	std::uint32_t __cdecl text(const char *) noexcept { return 1; }
	std::uint32_t __cdecl action() noexcept { return 1; }
	std::uint32_t __cdecl action_with_value(std::uint32_t) noexcept { return 1; }
	std::uint32_t __cdecl status(
	    kcd2o::kcse::client_status_view *) noexcept
	{
		return 1;
	}
	std::uint32_t __cdecl players(
	    kcd2o::kcse::remote_player_view *,
	    std::uint32_t) noexcept
	{
		return 0;
	}
	std::uint32_t __cdecl chat(
	    kcd2o::kcse::chat_entry_view *,
	    std::uint32_t) noexcept
	{
		return 0;
	}
	std::uint32_t __cdecl archetypes(
	    kcd2o::kcse::fixed_string *,
	    std::uint32_t) noexcept
	{
		return 0;
	}
	void __cdecl diagnostic_logging(std::uint32_t) noexcept {}
	std::uint32_t __cdecl resource_ui(char *, std::uint32_t) noexcept { return 0; }
	std::uint32_t __cdecl submit_resource_ui(
	    const kcd2o::kcse::resource_ui_event_request *) noexcept { return 1; }
}

int main()
{
	using namespace kcd2o::kcse;
	static_assert(std::is_standard_layout_v<fixed_string>);
	static_assert(std::is_trivially_copyable_v<fixed_string>);
	static_assert(std::is_standard_layout_v<connect_request>);
	static_assert(std::is_trivially_copyable_v<connect_request>);
	static_assert(std::is_standard_layout_v<runtime_status>);
	static_assert(std::is_trivially_copyable_v<runtime_status>);
	static_assert(std::is_standard_layout_v<client_status_view>);
	static_assert(std::is_trivially_copyable_v<client_status_view>);
	static_assert(std::is_standard_layout_v<remote_player_view>);
	static_assert(std::is_trivially_copyable_v<remote_player_view>);
	static_assert(std::is_standard_layout_v<chat_entry_view>);
	static_assert(std::is_trivially_copyable_v<chat_entry_view>);
	static_assert(std::is_standard_layout_v<client_api>);
	static_assert(std::is_trivially_copyable_v<client_api>);
	static_assert(std::is_standard_layout_v<resource_ui_event_request>);
	static_assert(std::is_trivially_copyable_v<resource_ui_event_request>);
#ifdef _WIN64
	static_assert(sizeof(fixed_string) == 64);
	static_assert(sizeof(connect_request) == 836);
	static_assert(sizeof(runtime_status) == 424);
	static_assert(sizeof(client_status_view) == 720);
	static_assert(sizeof(remote_player_view) == 88);
	static_assert(sizeof(chat_entry_view) == 344);
	static_assert(sizeof(resource_ui_event_request) == 33028);
	static_assert(sizeof(client_api) == 136);
#endif

	client_api valid{
	    sizeof(client_api),
	    kcd2o::kcd2o_version_major,
	    kcd2o::kcd2o_version_minor,
	    kcd2o::kcd2o_version_patch,
	    runtime,
	    connect,
	    disconnect,
	    text,
	    action_with_value,
	    text,
	    action,
	    action,
	    status,
	    players,
	    chat,
	    archetypes,
	    diagnostic_logging,
	    resource_ui,
	    submit_resource_ui};
	assert(compatible(&valid));
	static_assert(kcd2o::kcd2o_version == "0.1.4");
	static_assert(kcd2o::kcd2o_version_major == 0);
	static_assert(kcd2o::kcd2o_version_minor == 1);
	static_assert(kcd2o::kcd2o_version_patch == 4);

	auto wrong_version = valid;
	++wrong_version.version_patch;
	assert(!compatible(&wrong_version));

	auto short_struct = valid;
	short_struct.struct_size =
	    static_cast<std::uint32_t>(offsetof(client_api, copy_chat));
	assert(!compatible(&short_struct));

	auto future_struct = valid;
	++future_struct.struct_size;
	assert(!compatible(&future_struct));

	auto missing_function = valid;
	missing_function.copy_players = nullptr;
	assert(!compatible(&missing_function));
}
