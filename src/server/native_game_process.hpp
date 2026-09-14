#pragma once

#include "server/game_install.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace kcd2o::server
{
	struct native_game_launch_options
	{
		game_installation installation;
		std::filesystem::path server_config_path;
		std::string server_endpoint;
		std::string level_id;
		bool hide_window{true};
	};

	class native_game_process
	{
	  public:
		native_game_process();
		~native_game_process();

		native_game_process(const native_game_process &) = delete;
		native_game_process &operator=(const native_game_process &) = delete;
		native_game_process(native_game_process &&) noexcept;
		native_game_process &operator=(native_game_process &&) noexcept;

		void start(const native_game_launch_options &options);
		void stop() noexcept;
		void maintain_hidden_window() const noexcept;

		[[nodiscard]] bool running() const noexcept;
		[[nodiscard]] std::optional<std::uint32_t> exit_code() const noexcept;
		[[nodiscard]] std::uint32_t process_id() const noexcept;

	  private:
		struct implementation;
		std::unique_ptr<implementation> m_implementation;
	};
}
