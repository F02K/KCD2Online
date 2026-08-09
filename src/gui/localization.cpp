#include "gui/localization.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace big::ingame_ui
{
	namespace
	{
		std::string_view trim(std::string_view value)
		{
			while (!value.empty()
			    && std::isspace(static_cast<unsigned char>(value.front())))
			{
				value.remove_prefix(1);
			}
			while (!value.empty()
			    && std::isspace(static_cast<unsigned char>(value.back())))
			{
				value.remove_suffix(1);
			}
			return value;
		}

		bool valid_utf8(std::string_view text)
		{
			for (std::size_t offset = 0; offset < text.size();)
			{
				const auto lead = static_cast<unsigned char>(text[offset]);
				if (lead < 0x80)
				{
					++offset;
					continue;
				}
				std::size_t length{};
				std::uint32_t codepoint{};
				if ((lead & 0xE0) == 0xC0)
				{
					length = 2;
					codepoint = lead & 0x1F;
				}
				else if ((lead & 0xF0) == 0xE0)
				{
					length = 3;
					codepoint = lead & 0x0F;
				}
				else if ((lead & 0xF8) == 0xF0)
				{
					length = 4;
					codepoint = lead & 0x07;
				}
				else
				{
					return false;
				}
				if (offset + length > text.size())
					return false;
				for (std::size_t index = 1; index < length; ++index)
				{
					const auto continuation =
					    static_cast<unsigned char>(text[offset + index]);
					if ((continuation & 0xC0) != 0x80)
						return false;
					codepoint = (codepoint << 6) | (continuation & 0x3F);
				}
				const bool overlong = (length == 2 && codepoint < 0x80)
				    || (length == 3 && codepoint < 0x800)
				    || (length == 4 && codepoint < 0x10000);
				if (overlong || codepoint > 0x10FFFF
				    || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
				{
					return false;
				}
				offset += length;
			}
			return true;
		}

		std::string decode_escapes(std::string_view value)
		{
			std::string result;
			result.reserve(value.size());
			for (std::size_t index{}; index < value.size(); ++index)
			{
				if (value[index] != '\\' || index + 1 >= value.size())
				{
					result.push_back(value[index]);
					continue;
				}
				const auto escaped = value[index + 1];
				switch (escaped)
				{
				case 'n': result.push_back('\n'); break;
				case 'r': result.push_back('\r'); break;
				case 't': result.push_back('\t'); break;
				case '\\': result.push_back('\\'); break;
				default:
					result.push_back('\\');
					result.push_back(escaped);
					break;
				}
				++index;
			}
			return result;
		}

		bool load_file(
		    const std::filesystem::path &path,
		    std::unordered_map<std::string, std::string> &target,
		    std::string &error)
		{
			std::ifstream input(path, std::ios::binary);
			if (!input)
			{
				error = "language file is missing: " + path.string();
				return false;
			}
			std::string content{
			    std::istreambuf_iterator<char>(input),
			    std::istreambuf_iterator<char>()};
			if (content.starts_with("\xEF\xBB\xBF"))
				content.erase(0, 3);
			if (!valid_utf8(content))
			{
				error = "language file is not valid UTF-8: " + path.string();
				return false;
			}

			std::size_t line_number{};
			for (std::size_t offset = 0; offset <= content.size();)
			{
				++line_number;
				const auto end = content.find('\n', offset);
				const auto line = trim(std::string_view(content).substr(
				    offset,
				    end == std::string::npos ? std::string::npos : end - offset));
				offset = end == std::string::npos ? content.size() + 1 : end + 1;
				if (line.empty() || line.front() == '#' || line.front() == ';')
					continue;
				const auto separator = line.find('=');
				const auto key = trim(line.substr(0, separator));
				const auto value = separator == std::string_view::npos
				    ? std::string_view{}
				    : trim(line.substr(separator + 1));
				if (separator == std::string_view::npos || key.empty())
				{
					error = path.string() + ":" + std::to_string(line_number)
					    + ": expected key=value";
					return false;
				}
				if (!target.emplace(key, decode_escapes(value)).second)
				{
					error = path.string() + ":" + std::to_string(line_number)
					    + ": duplicate key " + std::string(key);
					return false;
				}
			}
			return true;
		}
	}

	std::string normalize_language(std::string_view language)
	{
		std::string normalized(trim(language));
		std::ranges::transform(
		    normalized,
		    normalized.begin(),
		    [](unsigned char character)
		    {
			    return character == '-' ? '_'
			                            : static_cast<char>(std::tolower(character));
		    });
		const auto separator = normalized.find_first_of("_.");
		const auto base = normalized.substr(0, separator);
		static const std::unordered_map<std::string_view, std::string_view> aliases{
		    {"english", "en"}, {"german", "de"}, {"deutsch", "de"},
		    {"french", "fr"}, {"spanish", "es"}, {"italian", "it"},
		    {"czech", "cs"}, {"polish", "pl"}, {"russian", "ru"},
		    {"portuguese", "pt"}, {"turkish", "tr"}, {"japanese", "ja"},
		    {"korean", "ko"}, {"chinese", "zh"}, {"dutch", "nl"}};
		if (const auto found = aliases.find(base); found != aliases.end())
			return std::string(found->second);
		return base.empty() ? "en" : base;
	}

	bool localization_catalog::load(
	    const std::filesystem::path &directory,
	    std::string_view language,
	    std::string &error)
	{
		std::unordered_map<std::string, std::string> fallback;
		if (!load_file(directory / "en.lang", fallback, error))
			return false;

		const auto normalized = normalize_language(language);
		auto merged = fallback;
		if (normalized != "en")
		{
			std::unordered_map<std::string, std::string> localized;
			const auto localized_path = directory / (normalized + ".lang");
			if (std::filesystem::is_regular_file(localized_path))
			{
				if (!load_file(localized_path, localized, error))
					return false;
				for (auto &[key, value] : localized)
					merged.insert_or_assign(std::move(key), std::move(value));
			}
		}
		m_language = normalized;
		m_text = std::move(merged);
		error.clear();
		return true;
	}

	std::string localization_catalog::text(std::string_view key) const
	{
		const auto found = m_text.find(std::string(key));
		return found == m_text.end()
		    ? "[[" + std::string(key) + "]]"
		    : found->second;
	}

	std::string localization_catalog::format(
	    std::string_view key,
	    std::initializer_list<format_argument> arguments) const
	{
		auto result = text(key);
		for (const auto &[name, value] : arguments)
		{
			const auto placeholder = "{" + std::string(name) + "}";
			for (std::size_t offset = 0;
			     (offset = result.find(placeholder, offset)) != std::string::npos;)
			{
				result.replace(offset, placeholder.size(), value);
				offset += value.size();
			}
		}
		return result;
	}

	const std::string &localization_catalog::language() const noexcept
	{
		return m_language;
	}
}
