#include "account/account_service.hpp"

#include "account/account_api.hpp"
#include "account/account_crypto.hpp"

#include <Windows.h>

#include <exception>
#include <stdexcept>
#include <utility>

namespace kcd2o::account
{
	account_service::account_service()
	{
		try
		{
			m_store = std::make_unique<account_store>();
			const auto &stored = m_store->value();
			m_account_id = stored.account_id;
			switch (stored.consent)
			{
			case consent_choice::undecided:
				m_state = account_state::undecided;
				break;
			case consent_choice::declined:
				m_state = account_state::declined;
				break;
			case consent_choice::accepted:
				if (stored.has_identity())
					m_state = account_state::ready;
				else
				{
					m_state = account_state::error;
					m_error_key = "account.error.incomplete";
				}
				break;
			}
		}
		catch (const std::exception &exception)
		{
			m_state = account_state::error;
			m_error_key = "account.error.local_store";
			const auto message = std::string("KCD2Online account store: ")
			    + exception.what() + "\n";
			OutputDebugStringA(message.c_str());
		}
	}

	account_service::~account_service()
	{
		m_worker.request_stop();
	}

	account_status account_service::status() const
	{
		std::scoped_lock lock(m_mutex);
		return {m_state, m_account_id, m_error_key, m_revision};
	}

	void account_service::accept(std::string service_url)
	{
		{
			std::scoped_lock lock(m_mutex);
			if (m_state == account_state::registering || !m_store)
				return;
			try
			{
				auto stored = m_store->value();
				stored.consent = consent_choice::accepted;
				m_store->replace(stored);
				if (stored.has_identity())
				{
					m_state = account_state::ready;
					m_account_id = stored.account_id;
					m_error_key.clear();
					bump_revision();
					return;
				}
				m_state = account_state::registering;
				m_error_key.clear();
				bump_revision();
			}
			catch (const std::exception &exception)
			{
				set_error("account.error.local_store", exception.what());
				return;
			}
		}

		try
		{
			m_worker = std::jthread(
			    [this, service_url = std::move(service_url)](std::stop_token stop)
			    {
				    try
				    {
					    auto credential = generate_credential();
					    const auto evidence = derive_device_evidence();
					    const auto registered = account_api(service_url).register_account(
					        credential.public_key_spki,
					        evidence,
					        credential.private_key_blob);
					    if (stop.stop_requested())
					        return;
					    account_record stored;
					    stored.consent = consent_choice::accepted;
					    stored.account_id = registered.account_id;
					    stored.credential_id = registered.credential_id;
					    stored.private_key_blob = std::move(credential.private_key_blob);
					    stored.recovery_code = registered.recovery_code;
					    m_store->replace(std::move(stored));
					    std::scoped_lock lock(m_mutex);
					    m_state = account_state::ready;
					    m_account_id = registered.account_id;
					    m_error_key.clear();
					    bump_revision();
				    }
				    catch (const std::exception &exception)
				    {
					    const std::string detail = exception.what();
					    const auto key = detail.find("already exists") != std::string::npos
					        ? "account.error.existing_device"
					        : "account.error.service";
					    std::scoped_lock lock(m_mutex);
					    set_error(key, detail);
				    }
			    });
		}
		catch (const std::exception &exception)
		{
			std::scoped_lock lock(m_mutex);
			set_error("account.error.service", exception.what());
		}
	}

	void account_service::decline()
	{
		std::scoped_lock lock(m_mutex);
		if (m_state == account_state::registering || !m_store)
			return;
		try
		{
			auto stored = m_store->value();
			stored.consent = consent_choice::declined;
			m_store->replace(std::move(stored));
			m_state = account_state::declined;
			m_error_key.clear();
			bump_revision();
		}
		catch (const std::exception &exception)
		{
			set_error("account.error.local_store", exception.what());
		}
	}

	void account_service::set_error(std::string key, std::string detail)
	{
		m_state = account_state::error;
		m_error_key = std::move(key);
		bump_revision();
		const auto message = std::string("KCD2Online account service: ")
		    + std::move(detail) + "\n";
		OutputDebugStringA(message.c_str());
	}

	void account_service::bump_revision() noexcept
	{
		++m_revision;
	}

	account_service &service()
	{
		static account_service instance;
		return instance;
	}
}
