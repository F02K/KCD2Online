#pragma once

extern "C"
{
#include <lua.h>
}

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kcd2o::scripting
{
	class lua_sandbox
	{
	public:
		using logger = std::function<void(std::string)>;
		using module_loader = std::function<std::optional<std::vector<std::byte>>(
		    std::string_view)>;

		lua_sandbox(
		    std::string name,
		    std::size_t memory_limit,
		    std::uint64_t instruction_limit,
		    logger log,
		    module_loader load_module,
		    void *owner = nullptr);
		~lua_sandbox();
		lua_sandbox(const lua_sandbox &) = delete;
		lua_sandbox &operator=(const lua_sandbox &) = delete;

		[[nodiscard]] lua_State *state() const noexcept;
		[[nodiscard]] void *owner() const noexcept;
		[[nodiscard]] const std::string &name() const noexcept;
		[[nodiscard]] std::size_t memory_used() const noexcept;
		[[nodiscard]] bool execute(
		    std::string_view source,
		    std::string_view chunk_name,
		    std::string &error);
		[[nodiscard]] bool call(
		    int registry_reference,
		    const std::function<int(lua_State *)> &push_arguments,
		    int result_count,
		    std::string &error);
		[[nodiscard]] int reference_function(int index);
		void release(int registry_reference) noexcept;
		void log(std::string message) const;

		[[nodiscard]] static lua_sandbox *from(lua_State *state) noexcept;
		[[nodiscard]] static nlohmann::json to_json(
		    lua_State *state,
		    int index,
		    std::size_t maximum_bytes = 32 * 1024);
		[[nodiscard]] static nlohmann::json parse_json(
		    std::string_view text,
		    std::size_t maximum_bytes = 32 * 1024);
		static void validate_json(const nlohmann::json &value);
		static void push_json(lua_State *state, const nlohmann::json &value);

	private:
		struct allocator_state
		{
			std::size_t used{};
			std::size_t limit{};
		};

		static void *allocate(
		    void *user,
		    void *pointer,
		    std::size_t old_size,
		    std::size_t new_size) noexcept;
		static void instruction_hook(lua_State *state, lua_Debug *debug);
		static int require_module(lua_State *state);
		static int print(lua_State *state);
		static int traceback(lua_State *state);
		void install_standard_library();
		[[nodiscard]] bool protected_call(
		    int arguments,
		    int results,
		    std::string &error);

		std::string m_name;
		allocator_state m_allocator;
		std::uint64_t m_instruction_limit{};
		std::uint64_t m_instruction_count{};
		logger m_log;
		module_loader m_load_module;
		void *m_owner{};
		lua_State *m_state{};
	};
}
