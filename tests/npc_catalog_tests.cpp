#include "npc/catalog.hpp"

#include <cassert>
#include <filesystem>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
	using namespace kcd2o::npc;
	const std::vector<catalog_document> documents{
	    {
	        "Libs/Tables/rpg/soul_archetype.xml",
	        R"xml(<database><soul_archetypes>
<soul_archetype race_id="0" gender_id="1" soul_archetype_id="0" soul_archetype_name="NPC"/>
<soul_archetype race_id="1" gender_id="0" soul_archetype_id="3" soul_archetype_name="Horse"/>
</soul_archetypes></database>)xml"},
	    {
	        "Libs/Tables/rpg/soul__test.xml",
	        R"xml(<database><souls>
<soul soul_archetype_id="0" soul_id="763db0bb-4469-497d-bdc9-712b3df91b5a" soul_name="ksta_additive_man_18" skald_character_name="char_GENERIC_MAN_COMMONER_18"/>
<soul soul_archetype_id="3" soul_id="11111111-1111-1111-1111-111111111111" soul_name="horse"/>
</souls></database>)xml"},
	    {
	        "Libs/Tables/rpg/soul__duplicate.xml",
	        R"xml(<database><souls>
<soul soul_archetype_id="0" soul_id="763db0bb-4469-497d-bdc9-712b3df91b5a" soul_name="duplicate"/>
</souls></database>)xml"}};

	catalog value;
	std::string error;
	assert(value.load_documents(documents, error));
	assert(error.empty());
	assert(value.size() == 1);
	assert(value.contains(default_soul_id));
	assert(!value.contains("11111111-1111-1111-1111-111111111111"));
	assert(value.normalize("missing") == default_soul_id);
	const auto *entry = value.find(default_soul_id);
	assert(entry);
	assert(entry->soul_name == "ksta_additive_man_18");
	assert(entry->character_id == "char_GENERIC_MAN_COMMONER_18");
	assert(entry->archetype_name == "NPC");
	assert(entry->gender == "1");

#ifdef KCD2Online_SOURCE_DIR
	catalog generated;
	assert(generated.load_json(
	    std::filesystem::path(KCD2Online_SOURCE_DIR)
	        / "data/npc_archetypes.json",
	    error));
	assert(generated.size() == supported_catalog_size);
	assert(generated.fingerprint() == supported_catalog_fingerprint);
	assert(*generated.find(default_soul_id)
	    == *generated.find(generated.normalize("unknown")));
#endif
	if (argc == 2)
	{
		catalog retail;
		assert(retail.load_tables_pak(argv[1], error));
		assert(retail.size() == supported_catalog_size);
		assert(retail.fingerprint() == supported_catalog_fingerprint);
	}
	return 0;
}
