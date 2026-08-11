#include "resources/sha256.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <limits>
#include <stdexcept>

namespace kcd2o::resources
{
	namespace
	{
		class algorithm_handle
		{
		public:
			algorithm_handle()
			{
				if (BCryptOpenAlgorithmProvider(
				        &m_value, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
					throw std::runtime_error("could not open SHA-256 provider");
			}

			~algorithm_handle()
			{
				if (m_value)
					BCryptCloseAlgorithmProvider(m_value, 0);
			}

			operator BCRYPT_ALG_HANDLE() const noexcept { return m_value; }

		private:
			BCRYPT_ALG_HANDLE m_value{};
		};

		class hash_handle
		{
		public:
			explicit hash_handle(BCRYPT_ALG_HANDLE algorithm)
			{
				if (BCryptCreateHash(
				        algorithm, &m_value, nullptr, 0, nullptr, 0, 0) < 0)
					throw std::runtime_error("could not create SHA-256 hash");
			}

			~hash_handle()
			{
				if (m_value)
					BCryptDestroyHash(m_value);
			}

			operator BCRYPT_HASH_HANDLE() const noexcept { return m_value; }

		private:
			BCRYPT_HASH_HANDLE m_value{};
		};
	}

	sha256_digest sha256(std::span<const std::byte> bytes)
	{
		algorithm_handle algorithm;
		hash_handle hash(algorithm);
		if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<ULONG>::max()))
			throw std::runtime_error("SHA-256 input is too large");
		if (!bytes.empty()
		    && BCryptHashData(
		           hash,
		           reinterpret_cast<PUCHAR>(
		               const_cast<std::byte *>(bytes.data())),
		           static_cast<ULONG>(bytes.size()),
		           0) < 0)
			throw std::runtime_error("could not update SHA-256 hash");
		sha256_digest result{};
		if (BCryptFinishHash(
		        hash,
		        reinterpret_cast<PUCHAR>(result.data()),
		        static_cast<ULONG>(result.size()),
		        0) < 0)
			throw std::runtime_error("could not finish SHA-256 hash");
		return result;
	}

	std::string sha256_hex(std::span<const std::byte> bytes)
	{
		constexpr char digits[] = "0123456789abcdef";
		const auto digest = sha256(bytes);
		std::string result;
		result.resize(digest.size() * 2);
		for (std::size_t index{}; index < digest.size(); ++index)
		{
			const auto value = std::to_integer<unsigned char>(digest[index]);
			result[index * 2] = digits[value >> 4];
			result[index * 2 + 1] = digits[value & 0x0F];
		}
		return result;
	}

	bool valid_sha256_hex(std::string_view value) noexcept
	{
		if (value.size() != 64)
			return false;
		for (const auto character : value)
			if (!((character >= '0' && character <= '9')
			        || (character >= 'a' && character <= 'f')))
				return false;
		return true;
	}
}
