#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace kcd2o
{
	class identity_store
	{
	public:
		explicit identity_store(std::filesystem::path path = default_path());

		[[nodiscard]] std::optional<std::string> token_for(
		    std::string_view server_id) const;
		void store(std::string server_id, std::string token);
		void erase(std::string_view server_id);
		[[nodiscard]] const std::filesystem::path &path() const;

		[[nodiscard]] static std::filesystem::path default_path();

	private:
		void load();
		void save() const;

		std::filesystem::path m_path;
		std::unordered_map<std::string, std::string> m_tokens;
	};
}
