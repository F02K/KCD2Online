#pragma once

#include "server/dashboard_config.hpp"

#include <functional>
#include <memory>
#include <string>

namespace kcd2o::server
{
	class dashboard_server
	{
	public:
		using snapshot_provider = std::function<std::string()>;

		dashboard_server(
		    dashboard_config config,
		    snapshot_provider provide_snapshot);
		~dashboard_server();
		dashboard_server(const dashboard_server &) = delete;
		dashboard_server &operator=(const dashboard_server &) = delete;

		void start();
		void stop();
		[[nodiscard]] std::string endpoint() const;

	private:
		class implementation;
		std::unique_ptr<implementation> m_impl;
	};
}
