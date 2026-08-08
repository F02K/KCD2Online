#include "npc/equipment_catalog.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <zip.h>

namespace
{
	struct temporary_directory
	{
		temporary_directory()
		{
			path = std::filesystem::temp_directory_path()
			    / ("kcd2o-equipment-"
			       + std::to_string(
			           std::chrono::steady_clock::now()
			               .time_since_epoch()
			               .count()));
			std::filesystem::create_directories(path);
		}

		~temporary_directory()
		{
			std::error_code error;
			std::filesystem::remove_all(path, error);
		}

		std::filesystem::path path;
	};

	void write_pak(
	    const std::filesystem::path &path,
	    const std::vector<kcd2o::npc::catalog_document> &documents)
	{
		std::filesystem::create_directories(path.parent_path());
		auto *archive = zip_open(path.string().c_str(), 6, 'w');
		assert(archive);
		for (const auto &document : documents)
		{
			assert(zip_entry_open(archive, document.source.c_str()) == 0);
			assert(zip_entry_write(
			           archive,
			           document.xml.data(),
			           document.xml.size())
			    == 0);
			assert(zip_entry_close(archive) == 0);
		}
		zip_close(archive);
	}
}

int main(int argc, char **argv)
{
	using namespace kcd2o::npc;
	const std::vector<catalog_document> documents{
	    {
	        "Libs/Tables/item/equipment_slot.xml",
	        R"xml(<database><EquipmentSlots>
<EquipmentSlot Id="12" Name="body_cloth_padded" BodyLayerTypeId="2" ArmorTypes="GambesonShort"/>
<EquipmentSlot Id="15" Name="body_plate" BodyLayerTypeId="5" ArmorTypes="Cuirass"/>
<EquipmentSlot Id="7" Name="boot" BodyLayerTypeId="3" ArmorTypes="BootsKnee"/>
<EquipmentSlot Id="99" Name="horse_body" BodyLayerTypeId="1" ArmorTypes="HorseBody"/>
</EquipmentSlots></database>)xml"},
	    {
	        "Libs/Tables/item/armor_type.xml",
	        R"xml(<database><armor_types>
<armor_type Id="1" Name="GambesonShort"/>
<armor_type Id="2" Name="Cuirass"/>
<armor_type Id="3" Name="BootsKnee"/>
<armor_type Id="4" Name="HorseBody"/>
</armor_types></database>)xml"},
	    {
	        "Libs/Tables/item/weapon_class.xml",
	        R"xml(<database><WeaponClasss>
<WeaponClass id="1" name="sword" equip_slot="PrimaryMainHand" is_twohanded="false"/>
<WeaponClass id="2" name="bow" equip_slot="SecondaryMainHand" is_twohanded="true"/>
</WeaponClasss></database>)xml"},
	    {
	        "Libs/Tables/item/item.xml",
	        R"xml(<database><ItemClasses>
<Armor Id="11111111-1111-1111-1111-111111111111" Clothing="GambesonShort"/>
<Armor Id="22222222-2222-2222-2222-222222222222" Clothing="Cuirass"/>
<Armor Id="33333333-3333-3333-3333-333333333333" Clothing="BootsKnee"/>
<Armor Id="44444444-4444-4444-4444-444444444444" Clothing="HorseBody"/>
<MeleeWeapon Id="55555555-5555-5555-5555-555555555555" Class="1"/>
<ItemAlias Id="b867dd0e-1bfe-40e9-b114-4b126a3ff1b0"
 SourceItemId="55555555-5555-5555-5555-555555555555"/>
<ItemAlias Id="99999999-9999-4999-8999-999999999999"
 SourceItemId="b867dd0e-1bfe-40e9-b114-4b126a3ff1b0"/>
<MissileWeapon Id="66666666-6666-6666-6666-666666666666" Class="2"/>
<Food Id="77777777-7777-7777-7777-777777777777"/>
</ItemClasses></database>)xml"}};

	equipment_catalog catalog;
	std::string error;
	assert(catalog.load_documents(documents, error));
	assert(error.empty());
	assert(catalog.size() == 7);
	const auto *native_padded_slot = catalog.find_slot(12);
	assert(native_padded_slot);
	assert(native_padded_slot->name == "body_cloth_padded");
	assert(native_padded_slot->layer == 2);
	assert(catalog.layer_for_slot("body_cloth_padded") == 2);
	assert(!catalog.find_slot(99));

	const auto *padded =
	    catalog.find("11111111-1111-1111-1111-111111111111");
	assert(padded);
	assert(padded->equipped_slot == "body_cloth_padded");
	assert(padded->layer == 2);
	assert(padded->weapon == weapon_class::none);

	const auto *plate =
	    catalog.find("22222222-2222-2222-2222-222222222222");
	assert(plate && plate->layer > padded->layer);

	const auto *sword =
	    catalog.find("55555555-5555-5555-5555-555555555555");
	assert(sword);
	assert(sword->equipped_slot == "PrimaryMainHand");
	assert(sword->weapon == weapon_class::one_handed);
	const auto *sword_alias =
	    catalog.find("b867dd0e-1bfe-40e9-b114-4b126a3ff1b0");
	assert(sword_alias);
	assert(sword_alias->equipped_slot == sword->equipped_slot);
	assert(sword_alias->layer == sword->layer);
	assert(sword_alias->weapon == sword->weapon);
	const auto *chained_sword_alias =
	    catalog.find("99999999-9999-4999-8999-999999999999");
	assert(chained_sword_alias);
	assert(chained_sword_alias->equipped_slot == sword->equipped_slot);
	assert(chained_sword_alias->weapon == sword->weapon);

	const auto *bow =
	    catalog.find("66666666-6666-6666-6666-666666666666");
	assert(bow);
	assert(bow->equipped_slot == "SecondaryMainHand");
	assert(bow->weapon == weapon_class::bow);

	assert(!catalog.find("44444444-4444-4444-4444-444444444444"));
	assert(!catalog.find("77777777-7777-7777-7777-777777777777"));

	temporary_directory installation;
	write_pak(installation.path / "Data" / "Tables.pak", documents);
	const auto mod = installation.path / "mods" / "visible_items";
	write_pak(
	    mod / "data" / "visible_items.pak",
	    {{
	        "Libs/Tables/item/item__visible_items.xml",
	        R"xml(<database><ItemClasses>
<Armor Id="88888888-8888-4888-8888-888888888888" Clothing="GambesonShort"/>
</ItemClasses></database>)xml"}});
	{
		std::ofstream log(installation.path / "kcd.log");
		log << "[Mod] Opening paks in mods/visible_items/data/*.pak\n";
	}
	equipment_catalog active_catalog;
	assert(active_catalog.load_game_install(installation.path, error));
	assert(active_catalog.size() == 8);
	const auto *modded =
	    active_catalog.find("88888888-8888-4888-8888-888888888888");
	assert(modded);
	assert(modded->equipped_slot == "body_cloth_padded");

	if (argc == 2)
	{
		equipment_catalog installed_catalog;
		assert(installed_catalog.load_tables_pak(argv[1], error));
		const auto *installed_alias =
		    installed_catalog.find(
		        "b867dd0e-1bfe-40e9-b114-4b126a3ff1b0");
		assert(installed_alias);
		assert(installed_alias->equipped_slot == "PrimaryMainHand");
		assert(installed_alias->weapon == weapon_class::one_handed);
	}
	return 0;
}
