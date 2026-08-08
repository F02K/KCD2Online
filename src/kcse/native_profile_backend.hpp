#pragma once

#include "multiplayer/profile_reconciler.hpp"
#include "kcse/native_entity_backend.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace wh::entitymodule
{
	class C_Inventory;
	class C_EquipmentManager;
}

namespace wh::rpgmodule
{
	class C_Soul;
}

namespace wh::playermodule
{
	class C_OutfitManager;
}

namespace kcd2o::kcse
{
	class native_profile_backend final : public profile_backend
	{
	public:
		explicit native_profile_backend(native_entity_backend &entities);

		void set_wire_identity(const protocol::PlayerProfile &profile);
		void reset();
		[[nodiscard]] bool ready(std::string &error) const;
		[[nodiscard]] std::optional<protocol::AvatarDescriptor>
		capture_avatar_visual(std::string &error) const;

		[[nodiscard]] std::optional<protocol::PlayerProfile> capture(
		    std::string &error) override;
		[[nodiscard]] bool validate_item(
		    const protocol::InventoryItem &item,
		    std::string &error) override;
		[[nodiscard]] int slot_layer(std::string_view slot) const override;
		[[nodiscard]] bool unequip(
		    std::string_view instance_id,
		    std::string &error) override;
		[[nodiscard]] bool remove_item(
		    std::string_view instance_id,
		    std::string &error) override;
		[[nodiscard]] bool create_item(
		    const protocol::InventoryItem &item,
		    std::string &error) override;
		[[nodiscard]] bool update_item(
		    const protocol::InventoryItem &item,
		    std::string &error) override;
		[[nodiscard]] bool set_money(
		    std::int64_t money,
		    std::uint32_t subunits,
		    std::string &error) override;
		[[nodiscard]] bool set_rpg_value(
		    bool skill,
		    const protocol::RpgValue &value,
		    std::string &error) override;
		[[nodiscard]] bool equip(
		    std::string_view instance_id,
		    std::string_view slot,
		    std::string &error) override;
		[[nodiscard]] bool set_quick_access_slots(
		    const protocol::PlayerProfile &profile,
		    std::string &error) override;
		[[nodiscard]] bool set_avatar_state(
		    const protocol::AvatarDescriptor &avatar,
		    std::string &error) override;
		[[nodiscard]] bool set_transform(
		    const protocol::TransformState &transform,
		    std::string &error) override;

	private:
		struct native_state
		{
			wh::rpgmodule::C_Soul *soul{};
			wh::entitymodule::C_Inventory *inventory{};
			wh::entitymodule::C_EquipmentManager *equipment{};
			wh::playermodule::C_OutfitManager *outfits{};
		};

		[[nodiscard]] std::optional<native_state> state(
		    std::string &error) const;
		native_entity_backend &m_entities;
		std::optional<protocol::PlayerProfile> m_wire_identity;
		std::optional<protocol::AvatarDescriptor> m_avatar_state;
	};
}
