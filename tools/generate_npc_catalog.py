#!/usr/bin/env python3
"""Generate a minimal local server avatar allowlist from Tables.pak."""

from __future__ import annotations

import argparse
import json
import pathlib
import xml.etree.ElementTree as ET
import zipfile

DEFAULT_SOUL_ID = "763db0bb-4469-497d-bdc9-712b3df91b5a"


def fingerprint(records: list[dict[str, str]]) -> str:
    value = 14695981039346656037
    for record in records:
        for key in (
            "soul_id",
            "soul_name",
            "character_id",
            "archetype_name",
            "gender",
            "source",
        ):
            for byte in record[key].encode("utf-8") + b"\0":
                value ^= byte
                value = value * 1099511628211 & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def is_soul_document(name: str) -> bool:
    lowered = name.lower()
    prefix = "libs/tables/rpg/soul"
    if not lowered.startswith(prefix) or not lowered.endswith(".xml"):
        return False
    suffix = lowered[len(prefix) :]
    return suffix == ".xml" or (suffix.startswith("__") and "/" not in suffix)


def generate(tables_pak: pathlib.Path) -> list[dict[str, str]]:
    with zipfile.ZipFile(tables_pak) as archive:
        archetypes = ET.fromstring(
            archive.read("Libs/Tables/rpg/soul_archetype.xml")
        )
        human = {
            node.attrib["soul_archetype_id"]: (
                node.attrib["soul_archetype_name"],
                node.attrib.get("gender_id", ""),
            )
            for node in archetypes.findall("./soul_archetypes/soul_archetype")
            if node.attrib.get("race_id") == "0"
        }
        result: list[dict[str, str]] = []
        seen: set[str] = set()
        for name in sorted(archive.namelist()):
            if not is_soul_document(name):
                continue
            root = ET.fromstring(archive.read(name))
            for soul in root.findall("./souls/soul"):
                kind = human.get(soul.attrib.get("soul_archetype_id", ""))
                soul_id = soul.attrib.get("soul_id", "")
                soul_name = soul.attrib.get("soul_name", "")
                if not kind or not soul_id or not soul_name or soul_id in seen:
                    continue
                seen.add(soul_id)
                result.append(
                    {
                        "soul_id": soul_id,
                        "soul_name": soul_name,
                        "character_id": soul.attrib.get(
                            "skald_character_name", ""
                        ),
                        "archetype_name": kind[0],
                        "gender": kind[1],
                        "source": name,
                    }
                )
    result.sort(key=lambda item: (item["soul_name"], item["soul_id"]))
    if DEFAULT_SOUL_ID not in {item["soul_id"] for item in result}:
        raise RuntimeError("the supported default Soul is missing")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("tables_pak", type=pathlib.Path)
    parser.add_argument(
        "--json",
        type=pathlib.Path,
        default=pathlib.Path("game_data/npc_archetypes.json"),
    )
    args = parser.parse_args()
    records = generate(args.tables_pak)

    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(
        json.dumps(
            {
                "schema_version": 2,
                "retail_build": "1308617_856",
                "catalog_fingerprint": fingerprint(records),
                "default_soul_id": DEFAULT_SOUL_ID,
                "soul_ids": sorted(record["soul_id"] for record in records),
            },
            indent=2,
            ensure_ascii=False,
        )
        + "\n",
        encoding="utf-8",
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
