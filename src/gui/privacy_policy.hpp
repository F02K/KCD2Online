#pragma once

#include <string_view>

namespace big::ingame_ui
{
	inline constexpr std::string_view privacy_title =
	    "KCD2ONLINE PRIVACY POLICY";

	inline constexpr std::string_view privacy_body = R"KCD2OPRIVACY(Version 1.0
Effective date: 12 August 2026

1. CONTROLLER AND CONTACT

The central KCD2Online services are operated privately from Germany under the name "KCD2Online".

For personal safety and security reasons, the Operator's private residential address is not published through the game client.

Privacy, account, and data-protection requests may be submitted through:

https://support.kingdom-online.cc

Direct operational contact:

Discord user: f02k_

Discord should not be used to transmit identity documents, passwords, private keys, recovery codes, access tokens, support-ticket access keys, or other confidential information.

Discord is an independent third-party service. If you contact the Operator through Discord, Discord may process your account information, messages, technical information, and other data under its own privacy terms.

The absence of a publicly displayed residential address does not restrict your statutory data-protection rights or your right to lodge a complaint with a competent supervisory authority.

2. SCOPE OF THIS POLICY

This Privacy Policy applies to the central KCD2Online services, including account registration and authentication; account profiles and recovery; multiplayer access-token validation; the central server directory; central security, administration, and moderation systems; server ownership and player-session records; the official support platform; and communications between the client, registered game servers, and the central service.

Independent community game servers may process additional information under their own responsibility. Such processing is not controlled by this Privacy Policy. Before using an independent server, you should review any privacy information provided by its operator.

KCD2Online is an independent community project and is not affiliated with, endorsed by, or operated by Warhorse Studios, Plaion, Deep Silver, Embracer Group, Valve, Epic Games, GOG, or another third-party platform. Those organisations and platforms process personal data independently under their own privacy policies.

3. PERSONAL DATA WE PROCESS

3.1 Account and profile data

We may process your pseudonymous KCD2Online account ID; optional username and display name; selected language or locale; network role and permissions; account creation, update, and last-login timestamps; account status, restrictions, suspensions, and bans; and notifications associated with your account.

An account can be created without providing a real name, postal address, telephone number, or email address. You must not use profile information to impersonate another person.

3.2 Authentication and credential data

KCD2Online uses cryptographic credentials instead of a conventional account password. We may process credential IDs, public cryptographic keys, credential labels, credential creation and last-use timestamps, short-lived authentication challenges, submitted cryptographic signatures, access-token identifiers and audiences, token issue and expiry times, session identifiers, revocation information, and account token-version information.

Your private credential key remains on your device and is not intentionally transmitted to the central service. Access tokens are short-lived but must be treated as confidential while valid.

3.3 Recovery data

When an account is created or recovered, KCD2Online generates a recovery code. The readable code is returned to the client when created. The central database stores a cryptographic hash rather than the readable recovery code.

When recovery is used, the submitted code is temporarily processed to verify it. A successfully used code becomes invalid and is replaced. Never publish a recovery code or submit it through Discord, a community server, or an ordinary support message.

3.4 Device identification

The Windows client creates a cryptographic digest from stable device characteristics that may include the Windows MachineGuid, the system-volume identifier, and the SMBIOS system UUID where available.

The underlying identifiers are combined and hashed locally. The central service receives the resulting digest and transforms it again using a secret server-side keyed hash. The result is stored as a pseudonymous device tag.

The raw Windows MachineGuid, volume identifier, and SMBIOS UUID are not intentionally transmitted to or stored by the central service. The resulting device tag is nevertheless a stable pseudonymous identifier and is treated as personal data.

Device tags are used to associate credentials with a trusted device, protect accounts, prevent unauthorised duplicate registration, detect compromised credentials, investigate abuse, and reduce circumvention of security controls or valid restrictions.

3.5 Network and security data

When a client, browser, or game server communicates with KCD2Online, we may process IP addresses; request times, endpoints, results, and error codes; associated account, credential, session, or server identifiers; registration, login, recovery, and profile-change events; administrative and moderation events; rate-limit information; session creation, use, expiry, and revocation; and technical error information.

IP addresses may be stored with account sessions, support requests, and security or audit events. Application logs are not intended to contain private keys, readable recovery codes, passwords, complete access tokens, or support-ticket access keys.

3.6 Multiplayer and game-server data

When you authenticate to or play on a registered server, we may process the server requested; authentication and validation times; account and credential IDs; display name; network role and permissions; server roles and whitelist status; restrictions; online presence; session start, last-observed, and end times; session count; and server ownership or administration relationships.

Registered game servers may report active and owner account IDs to the central service. A selected server validating its token may receive your account ID, display name, network role, relevant permissions, whitelist status, and applicable chat or voice restrictions. A token issued for one server cannot be successfully validated for another server.

3.7 Server-directory data

If you register or operate a server, we may process its ID, name, advertised address, software version, level, player counts, password-protection status, API-key hash, heartbeat and last-seen timestamps, linked owner account IDs, and administrative restriction information.

Information intentionally submitted to the public directory, including the server name, address, version, level, capacity, and password-protection status, is publicly visible.

3.8 Support data

When you submit a support request, we may process your email address where supplied; account, reported-account, and related-server IDs; request category, subject, description, messages, status, priority, staff assignment, timestamps, the ticket access-key hash, and related security and audit information.

An email address is required for support-assisted account recovery and privacy requests. It is not required for an ordinary game account. Do not include passwords, private keys, recovery codes, access tokens, unnecessary real-world identity information, or unlawful material in a support request.

3.9 Moderation and report data

For central moderation and appeals, we may process reports, alleged-conduct descriptions, account, server, and session references, relevant technical evidence, case status and priority, internal notes, restrictions, decisions, reasons, appeals, staff identities, timestamps, and audit records.

Reports may contain information received from other users, registered servers, server operators, or authorised staff. Only information reasonably necessary to review the matter should be submitted.

We do not request special-category personal data or information about criminal convictions. If such information is received incidentally, it will be restricted, removed, or processed only where a valid legal basis permits or requires it.

4. PURPOSES AND LEGAL BASES

We process account, authentication, credential, profile, multiplayer-access, and server-directory data where necessary to provide KCD2Online and perform the Terms of Service. The legal basis is Article 6(1)(b) GDPR.

We process device tags, IP addresses, session information, audit records, multiplayer activity, and security information where necessary for legitimate interests under Article 6(1)(f) GDPR. These interests include protecting accounts and credentials; maintaining network and information security; preventing unauthorised access; detecting abuse, fraud, cheating, and restriction evasion; maintaining a reliable directory; enforcing the Terms; protecting users and server operators; investigating incidents; and establishing, exercising, or defending legal claims.

We process support requests under Article 6(1)(b) or Article 6(1)(f) GDPR, depending on the request. We process moderation reports and appeals under Article 6(1)(f) GDPR. Where processing is required to comply with a legal obligation or lawful authority request, the basis is Article 6(1)(c) GDPR.

We do not sell or rent personal data. We do not use it for targeted advertising, data brokerage, or cross-service behavioural marketing. The client does not contain third-party advertising trackers or behavioural analytics.

5. SOURCES OF PERSONAL DATA

We receive personal data from you; your KCD2Online client; registered game servers you access; server operators; reporting users; authorised support, moderation, and administration staff; hosting and security systems; and authorities or other persons involved in a legal matter where applicable.

Server-supplied information may not always be independently verified by the central service.

6. RECIPIENTS

Personal data may be disclosed to authorised KCD2Online personnel; registered game servers selected by the user; Hetzner Online GmbH as infrastructure provider; Cloudflare and its affiliated providers for network delivery, availability, and abuse protection; professional advisers where necessary for legal claims; authorities where disclosure is legally required or permitted; and Discord if you voluntarily contact the Operator there.

Staff access is limited according to assigned roles and operational necessity. Independent community-server operators are responsible for data they independently collect. They do not receive unrestricted access to the central account database. KCD2Online does not sell personal data.

)KCD2OPRIVACY"
R"KCD2OPRIVACY(7. HOSTING AND INTERNATIONAL TRANSFERS

The central backend and primary database are hosted in Nuremberg, Germany, on infrastructure provided by:

Hetzner Online GmbH
Industriestrasse 25
91710 Gunzenhausen
Germany

The primary database is stored within the European Union. Hetzner processes infrastructure and hosting data on behalf of the Operator.

Further information: https://docs.hetzner.com/general/company-and-policy/data-protection-at-hetzner/

The public support service uses Cloudflare for network delivery, availability, and protection against malicious traffic. Cloudflare may process IP addresses, request metadata, transmitted content, and security events. Cloudflare operates a global network, so technical information may be processed outside the European Economic Area.

Where personal data are transferred outside the EEA, the transfer is protected through an applicable adequacy decision, the European Commission's Standard Contractual Clauses, or another legally recognised safeguard.

Further information: https://www.cloudflare.com/privacypolicy/

If you voluntarily use Discord, Discord may process the communication outside the EEA under its own privacy terms.

8. COOKIES AND LOCAL STORAGE

The game client does not use browser cookies. The public support area does not intentionally use advertising or analytics cookies. Cloudflare may use technically necessary cookies or similar mechanisms for security and availability.

The restricted staff area uses a strictly necessary authentication cookie. It is used only for authorised staff, is HTTP-only, uses SameSite protection, is not used for advertising, and expires after the configured authentication period or logout.

9. RETENTION

Personal data are retained only for as long as reasonably necessary. The standard periods are:

1) expired authentication challenges: no later than 24 hours after expiry;
2) access tokens: the configured short validity period, currently ten minutes;
3) expired or revoked account sessions, including stored IP addresses: 90 days;
4) consumed recovery-credential hashes: 30 days;
5) raw server-player sessions: 90 days;
6) aggregated server activity: 12 months after the last relevant activity;
7) ordinary security and audit events: 180 days;
8) ordinary support and account-recovery tickets: 12 months after closure;
9) moderation cases, appeals, and unlawful-content reports: up to three years after closure;
10) ended server-ownership relationships: up to three years;
11) active sanctions and minimum enforcement information: for the duration of the sanction; and
12) account, profile, credential, and active recovery information: while the account remains active.

Following approved account deletion, data not subject to an exception are deleted or irreversibly anonymised. Information may be retained longer for an active security incident, unresolved matter, legal claim, statutory obligation, lawful authority request, protection of another person, or prevention of repeated serious abuse.

A restricted pseudonymous account or device identifier may be retained where necessary to enforce a valid restriction. Continued necessity will be reviewed periodically.

If encrypted backups are maintained, deleted information may remain there for up to an additional 30 days and will only be restored for disaster recovery.

10. ACCOUNT DELETION

You may request deletion directly through Account > Privacy & Data or through https://support.kingdom-online.cc.

In-game deletion requires a freshly authenticated account-management session and entry of the complete account ID. The local credential is removed only after the central service confirms deletion.

Deletion normally removes or anonymises profile information, credentials, sessions, recovery credentials, ordinary multiplayer activity, and information no longer necessary. Some information may remain where required for security, active sanctions, dispute handling, legal claims, or protection of others. An account with an active restriction must request an individual erasure review through support.

Central deletion does not delete information independently stored by community-server operators. Contact the relevant operator about its records.

11. YOUR RIGHTS

Subject to applicable conditions and exceptions, you may have the right to obtain confirmation and access; correct inaccurate information; request erasure or restriction; receive applicable data in a portable format; object to processing based on Article 6(1)(f) GDPR; withdraw consent where processing is based on consent; and lodge a complaint with a competent data-protection authority.

You can create a machine-readable JSON export through Account > Privacy & Data or submit a request through https://support.kingdom-online.cc.

We may request proportionate verification. Never send a private key or recovery code as proof. Requests will normally be answered within one month and may be extended where legally permitted. You may complain to a supervisory authority in the EU Member State of your habitual residence, workplace, or the alleged infringement.

12. REQUIRED AND OPTIONAL INFORMATION

An account ID, public credential key, credential ID, device-evidence digest, pseudonymous device tag, and necessary authentication, session, network, and security information are required to create and operate an account. Without them, multiplayer access cannot be provided.

Username and display name are optional. Email is not required for an ordinary game account but is required for support-assisted recovery and privacy requests. Provide only information necessary for support or moderation.

13. AUTOMATED CONTROLS

Automated systems validate credentials and signatures, enforce rate limits, detect whether a device is associated with an account, validate tokens and sessions, verify memberships and restrictions, reject invalid access, apply active communication restrictions, and decide whether a server appears in the directory.

A device tag already associated with an account may prevent another registration on that device. These controls may reject registration, login, requests, or sessions. They are not used for advertising, credit scoring, or personality profiling. Final central moderation decisions are not intended to be solely automated. Human review can be requested through support.

14. SECURITY

Protective measures include cryptographic credentials, local private keys, hashed recovery codes and server keys, short-lived tokens, pseudonymous device identifiers, role-based staff access, protected staff cookies, rate limits, audit records, restricted administration, and infrastructure security.

No storage or transmission method guarantees absolute security. You are responsible for protecting private keys, recovery codes, access tokens, and ticket access keys. Contact support promptly if credentials may be compromised.

15. DATA BREACHES

Where a personal-data breach creates a reportable risk, we will notify the competent supervisory authority as required. Where it is likely to create a high risk, affected persons will also be informed where required and reasonably possible.

Security incidents may be reported at https://support.kingdom-online.cc. Do not publicly disclose active credentials or exploit details that create an immediate risk.

16. CHILDREN

KCD2Online is intended for users permitted to access the base game and multiplayer service under applicable law and platform rules. We do not knowingly request real names, dates of birth, school information, or other information specifically intended to identify children.

If personal data were improperly submitted by or about a child, we may restrict the account and delete the information unless retention is legally required or necessary to protect a person.

17. CHANGES TO THIS POLICY

We may update this Policy when the service, processing, infrastructure, retention periods, legal requirements, or safeguards change. Material changes will be announced through the Service or another appropriate channel before taking effect where reasonably possible.

The version and effective date identify the applicable text. Continued use alone will not be treated as consent where consent is legally required.

18. CONTACT SUMMARY

Privacy, account, deletion, access, correction, and portability requests:

https://support.kingdom-online.cc

Direct operational contact:

Discord user: f02k_

Hosting provider: Hetzner Online GmbH, Nuremberg, Germany

Network and support-service protection: Cloudflare

Never send passwords, private keys, recovery codes, access tokens, or support-ticket access keys through Discord or an ordinary support message.)KCD2OPRIVACY";
}
