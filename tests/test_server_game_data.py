from __future__ import annotations

import json
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path

from tools.generate_server_game_data import (
    DEFAULT_SOUL_ID,
    GAME_BIN_RELATIVE,
    GameDataError,
    generate_server_game_data,
)


def _write_pe(path: Path) -> None:
    image = bytearray(512)
    image[0:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x80)
    image[0x80:0x84] = b"PE\0\0"
    file_header = 0x84
    struct.pack_into("<H", image, file_header, 0x8664)
    struct.pack_into("<I", image, file_header + 4, 0x12345678)
    struct.pack_into("<H", image, file_header + 16, 0xF0)
    optional_header = file_header + 20
    struct.pack_into("<H", image, optional_header, 0x20B)
    struct.pack_into("<I", image, optional_header + 56, 0x05600000)
    path.parent.mkdir(parents=True)
    path.write_bytes(image)


def _write_tables(path: Path) -> None:
    path.parent.mkdir(parents=True)
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(
            "Libs/Tables/rpg/soul_archetype.xml",
            '<root><soul_archetypes><soul_archetype race_id="0" '
            'gender_id="1" soul_archetype_id="0" '
            'soul_archetype_name="NPC"/></soul_archetypes></root>',
        )
        archive.writestr(
            "Libs/Tables/rpg/soul.xml",
            '<root><souls><soul soul_archetype_id="0" '
            f'soul_id="{DEFAULT_SOUL_ID}" soul_name="Default" '
            'skald_character_name="char_default"/></souls></root>',
        )


def _write_level(path: Path) -> None:
    path.parent.mkdir(parents=True)
    objects = """<Objects>
      <Entity Name="Henry's neighbour" Pos="1,2,3" Rotate="1,0,0,0"
        EntityClass="NPC" EntityId="1" EntityGuid="00000001-0002-0003"
        EditorLayer="Main/npc" />
      <Entity Name="Horse" Pos="4,5,6" EntityClass="Horse" EntityId="2"
        EntityGuid="00000002-0002-0003" EditorLayer="Main/animals" />
      <Entity Name="Herd" Pos="7,8,9" EntityClass="AnimalSpawner" EntityId="3"
        EntityGuid="00000003-0002-0003" EditorLayer="Main/spawners">
        <Properties archetype="cattle" count="4" />
      </Entity>
      <Entity Name="UI Horse" EntityClass="InventoryDummyHorse" EntityId="4"
        EntityGuid="00000004-0002-0003" />
    </Objects>"""
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("objects_mission0.xml", objects)


def _game_root(root: Path) -> Path:
    game = root / "KingdomComeDeliverance2"
    _write_pe(game / GAME_BIN_RELATIVE / "WHGame.dll")
    (game / GAME_BIN_RELATIVE / "KingdomCome.exe").write_bytes(b"game")
    _write_tables(game / "Data" / "Tables.pak")
    for level in ("trosecko", "kutnohorsko", "klaster"):
        _write_level(game / "Data" / "Levels" / level / "level.pak")
    return game


class ServerGameDataTests(unittest.TestCase):
    def test_generates_minimal_server_metadata_without_game_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = _game_root(root)
            output = root / "server_game_data"

            output.mkdir()
            (output / "WHGame.dll").write_bytes(b"stale-game-code")
            generate_server_game_data(game, output)

            self.assertFalse((output / "WHGame.dll").exists())
            archetypes = json.loads(
                (output / "npc_archetypes.json").read_text(encoding="utf-8")
            )
            self.assertEqual(archetypes["schema_version"], 2)
            self.assertEqual(archetypes["default_soul_id"], DEFAULT_SOUL_ID)
            self.assertEqual(archetypes["soul_ids"], [DEFAULT_SOUL_ID])
            self.assertNotIn("archetypes", archetypes)

            world = json.loads(
                (output / "npc_world_catalog.json").read_text(encoding="utf-8")
            )
            self.assertEqual([level["level_id"] for level in world["levels"]], ["2", "3", "4"])
            for level in world["levels"]:
                self.assertEqual(
                    {npc["kind"] for npc in level["npcs"]}, {"human", "animal"}
                )
                self.assertTrue(
                    all(set(npc) == {"entity_guid", "kind"} for npc in level["npcs"])
                )
                self.assertNotIn("animal_spawners", level)

            manifest = json.loads(
                (output / "content_manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(manifest["whgame"]["timestamp"], 0x12345678)
            self.assertEqual(manifest["whgame"]["image_size"], 0x05600000)
            self.assertEqual(len(manifest["content_fingerprint"]), 64)
            self.assertEqual(len(manifest["sources"]), 5)
            self.assertNotIn(
                "WHGame.dll",
                {entry["path"] for entry in manifest["generated_files"]},
            )

    def test_rejects_a_non_pe_whgame(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = _game_root(root)
            (game / GAME_BIN_RELATIVE / "WHGame.dll").write_bytes(b"not a dll")
            with self.assertRaisesRegex(GameDataError, "not a PE image"):
                generate_server_game_data(game, root / "output")


if __name__ == "__main__":
    unittest.main()
