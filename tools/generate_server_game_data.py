#!/usr/bin/env python3
"""Generate deterministic dedicated-server metadata from a KCD2 installation."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile
from dataclasses import dataclass
from typing import Iterable, Optional, Sequence

try:
    from .generate_npc_catalog import DEFAULT_SOUL_ID, fingerprint, generate
except ImportError:
    from generate_npc_catalog import DEFAULT_SOUL_ID, fingerprint, generate


GAME_BIN_RELATIVE = pathlib.Path("Bin") / "Win64MasterMasterSteamPGO"
PRODUCTION_LEVELS = (
    ("2", "trosecko"),
    ("3", "kutnohorsko"),
    ("4", "klaster"),
)
HUMAN_ENTITY_CLASSES = frozenset(("npc", "npc_female"))
ANIMAL_ENTITY_CLASSES = frozenset(
    (
        "cattlecow",
        "chickens",
        "chickensbrownlight",
        "chickenswhite",
        "dog",
        "hare",
        "horse",
        "pig",
        "reddeerdoe",
        "reddeerstag",
        "roedeerbuck",
        "roedeerhind",
        "sheepewe",
        "sheepram",
        "wilddog",
        "wolf",
    )
)


class GameDataError(RuntimeError):
    """An actionable server-game-data generation failure."""


def _bundled_tool(name: str) -> Optional[pathlib.Path]:
    bundle_root = getattr(sys, "_MEIPASS", None)
    if bundle_root is None:
        return None
    candidate = pathlib.Path(bundle_root) / name
    return candidate if candidate.is_file() else None


def _detect_game_root() -> Optional[pathlib.Path]:
    try:
        from tools.build_tui.core import detect_game_root
    except ImportError:
        try:
            from build_tui.core import detect_game_root
        except ImportError:
            return None
    return detect_game_root()


def _normalize_game_root(path: pathlib.Path) -> pathlib.Path:
    candidate = path.expanduser().resolve()
    executable_name = "KingdomCome.exe"
    if candidate.is_file() or candidate.name.lower() == executable_name.lower():
        candidate = candidate.parent
    if (
        candidate.name.lower() == GAME_BIN_RELATIVE.name.lower()
        and (candidate / executable_name).is_file()
    ):
        return candidate.parents[1]
    return candidate


def _default_output() -> pathlib.Path:
    if getattr(sys, "frozen", False):
        return pathlib.Path(sys.executable).resolve().parent / "game_data"
    return pathlib.Path.cwd() / "game_data"


def _run_signature_audit(game_root: pathlib.Path, tool: pathlib.Path) -> None:
    whgame = game_root / GAME_BIN_RELATIVE / "WHGame.dll"
    try:
        subprocess.run([str(tool), str(whgame)], check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise GameDataError(f"WHGame.dll signature audit failed: {exc}") from exc


@dataclass(frozen=True)
class PeIdentity:
    timestamp: int
    image_size: int


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _pe_identity(path: pathlib.Path) -> PeIdentity:
    try:
        with path.open("rb") as stream:
            if stream.read(2) != b"MZ":
                raise GameDataError(f"{path} is not a PE image")
            stream.seek(0x3C)
            pe_offset_raw = stream.read(4)
            if len(pe_offset_raw) != 4:
                raise GameDataError(f"{path} has a truncated DOS header")
            pe_offset = struct.unpack("<I", pe_offset_raw)[0]
            stream.seek(pe_offset)
            if stream.read(4) != b"PE\0\0":
                raise GameDataError(f"{path} has no PE signature")
            file_header = stream.read(20)
            if len(file_header) != 20:
                raise GameDataError(f"{path} has a truncated PE file header")
            timestamp = struct.unpack_from("<I", file_header, 4)[0]
            optional_size = struct.unpack_from("<H", file_header, 16)[0]
            optional_header = stream.read(optional_size)
            if len(optional_header) != optional_size or optional_size < 60:
                raise GameDataError(f"{path} has a truncated PE optional header")
            magic = struct.unpack_from("<H", optional_header)[0]
            if magic not in (0x10B, 0x20B):
                raise GameDataError(f"{path} has an unsupported PE optional header")
            image_size = struct.unpack_from("<I", optional_header, 56)[0]
            if timestamp == 0 or image_size == 0:
                raise GameDataError(f"{path} has an empty build identity")
            return PeIdentity(timestamp, image_size)
    except OSError as exc:
        raise GameDataError(f"could not read {path}: {exc}") from exc


def _valid_entity_guid(value: str) -> bool:
    parts = value.split("-")
    if tuple(map(len, parts)) != (8, 4, 4):
        return False
    try:
        return int("".join(parts), 16) != 0
    except ValueError:
        return False


def _level_xml_entries(archive: zipfile.ZipFile) -> Iterable[zipfile.ZipInfo]:
    for info in sorted(archive.infolist(), key=lambda entry: entry.filename.lower()):
        lowered = info.filename.lower()
        if lowered == "objects_mission0.xml" or (
            lowered.startswith("layers/") and lowered.endswith(".xml")
        ):
            yield info


def _read_level_entry(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> bytes:
    try:
        return archive.read(info)
    except zipfile.BadZipFile as first_error:
        # Shipping level.pak files use backslashes in some local headers while
        # their central directory exposes normalized forward slashes. Python's
        # zipfile rejects that harmless mismatch unless we compare against the
        # spelling actually stored in the local header.
        original = info.orig_filename
        info.orig_filename = original.replace("/", "\\")
        try:
            return archive.read(info)
        except zipfile.BadZipFile:
            raise first_error
        finally:
            info.orig_filename = original


def _catalog_level(level_id: str, level_name: str, pak: pathlib.Path) -> dict:
    by_guid: dict[str, dict] = {}
    try:
        with zipfile.ZipFile(pak) as archive:
            for info in _level_xml_entries(archive):
                name = info.filename
                try:
                    payload = _read_level_entry(archive, info)
                    root = ET.fromstring(payload)
                except (KeyError, ET.ParseError) as exc:
                    raise GameDataError(f"could not parse {name} in {pak}: {exc}") from exc
                for node in root.findall(".//Entity"):
                    entity_class = node.attrib.get("EntityClass", "")
                    lowered_class = entity_class.lower()
                    if lowered_class in HUMAN_ENTITY_CLASSES:
                        kind = "human"
                    elif lowered_class in ANIMAL_ENTITY_CLASSES:
                        kind = "animal"
                    else:
                        continue
                    guid = node.attrib.get("EntityGuid", "").lower()
                    if not _valid_entity_guid(guid):
                        continue
                    entry = {
                        "entity_guid": guid,
                        "kind": kind,
                    }
                    previous = by_guid.get(guid)
                    if previous is not None and previous["kind"] != kind:
                        raise GameDataError(
                            f"level {level_name} reuses NPC GUID {guid} with incompatible kinds"
                        )
                    by_guid.setdefault(guid, entry)
    except (OSError, zipfile.BadZipFile) as exc:
        raise GameDataError(f"could not scan level archive {pak}: {exc}") from exc

    npcs = sorted(by_guid.values(), key=lambda item: item["entity_guid"])
    return {
        "level_id": level_id,
        "npcs": npcs,
    }


def _mod_paks(game_root: pathlib.Path) -> tuple[pathlib.Path, ...]:
    mods = next(
        (
            candidate
            for candidate in (game_root / "Mods", game_root / "mods")
            if candidate.is_dir()
        ),
        None,
    )
    if mods is None:
        return ()
    return tuple(
        sorted(
            (
                path
                for path in mods.rglob("*")
                if path.is_file() and path.suffix.lower() == ".pak"
            ),
            key=lambda path: path.as_posix().lower(),
        )
    )


def _source_record(game_root: pathlib.Path, path: pathlib.Path) -> dict:
    try:
        relative = path.relative_to(game_root).as_posix()
        size = path.stat().st_size
    except (OSError, ValueError) as exc:
        raise GameDataError(f"could not inspect source file {path}: {exc}") from exc
    return {"path": relative, "size": size, "sha256": _sha256(path)}


def _write_json(path: pathlib.Path, value: object) -> None:
    path.write_text(
        json.dumps(value, indent=2, ensure_ascii=False, sort_keys=False) + "\n",
        encoding="utf-8",
    )


def _replace_directory(staging: pathlib.Path, output: pathlib.Path) -> None:
    previous = output.with_name(output.name + ".previous")
    if previous.exists():
        shutil.rmtree(previous)
    if output.exists():
        os.replace(output, previous)
    try:
        os.replace(staging, output)
    except Exception:
        if previous.exists() and not output.exists():
            os.replace(previous, output)
        raise
    if previous.exists():
        shutil.rmtree(previous)


def generate_server_game_data(
    game_root: pathlib.Path,
    output: pathlib.Path,
    property_catalog_tool: Optional[pathlib.Path] = None,
) -> pathlib.Path:
    game_root = game_root.resolve()
    output = output.resolve()
    whgame = game_root / GAME_BIN_RELATIVE / "WHGame.dll"
    tables = game_root / "Data" / "Tables.pak"
    level_paks = tuple(
        (level_id, level_name, game_root / "Data" / "Levels" / level_name / "level.pak")
        for level_id, level_name in PRODUCTION_LEVELS
    )
    required = (whgame, tables, *(pak for _, _, pak in level_paks))
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise GameDataError("game installation is missing required files:\n" + "\n".join(missing))

    identity = _pe_identity(whgame)
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = pathlib.Path(
        tempfile.mkdtemp(prefix=output.name + ".", dir=str(output.parent))
    )
    try:
        archetypes = generate(tables)
        _write_json(
            staging / "npc_archetypes.json",
            {
                "schema_version": 2,
                "retail_build": "1308617_856",
                "catalog_fingerprint": fingerprint(archetypes),
                "default_soul_id": DEFAULT_SOUL_ID,
                # The dedicated server only validates configured avatar IDs.
                # Descriptive retail fields stay in the local game installation.
                "soul_ids": sorted(record["soul_id"] for record in archetypes),
            },
        )

        levels = [
            _catalog_level(level_id, level_name, pak)
            for level_id, level_name, pak in level_paks
        ]
        _write_json(
            staging / "npc_world_catalog.json",
            {
                "schema_version": 1,
                "identity": "level_id:authored_entity_guid",
                "levels": levels,
            },
        )

        if property_catalog_tool is not None:
            property_catalog_tool = property_catalog_tool.resolve()
            if not property_catalog_tool.is_file():
                raise GameDataError(
                    f"property catalog tool is missing: {property_catalog_tool}"
                )
            try:
                subprocess.run(
                    [str(property_catalog_tool), str(game_root), "--all", str(staging)],
                    check=True,
                )
            except (OSError, subprocess.CalledProcessError) as exc:
                raise GameDataError(f"property catalog generation failed: {exc}") from exc

        source_paths = [whgame, tables, *(pak for _, _, pak in level_paks), *_mod_paks(game_root)]
        sources = [_source_record(game_root, path) for path in source_paths]
        fingerprint_input = json.dumps(
            sources, separators=(",", ":"), sort_keys=True
        ).encode("utf-8")
        generated = []
        for path in sorted(staging.iterdir(), key=lambda candidate: candidate.name.lower()):
            if path.is_file() and path.name != "content_manifest.json":
                generated.append(
                    {"path": path.name, "size": path.stat().st_size, "sha256": _sha256(path)}
                )
        _write_json(
            staging / "content_manifest.json",
            {
                "schema_version": 1,
                "content_fingerprint": hashlib.sha256(fingerprint_input).hexdigest(),
                "whgame": {
                    "timestamp": identity.timestamp,
                    "image_size": identity.image_size,
                    "sha256": _sha256(whgame),
                },
                "sources": sources,
                "generated_files": generated,
            },
        )
        _replace_directory(staging, output)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    return output


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-root", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--property-catalog-tool", type=pathlib.Path)
    parser.add_argument("--signature-audit-tool", type=pathlib.Path)
    options = parser.parse_args(argv)
    interactive = not argv if argv is not None else len(sys.argv) == 1
    try:
        game_root = options.game_root
        if game_root is None:
            game_root = _detect_game_root()
        if game_root is None:
            entered = input(
                "KCD2 was not auto-detected. Enter the KingdomComeDeliverance2 folder: "
            ).strip().strip('"')
            if not entered:
                raise GameDataError("no KCD2 installation directory was provided")
            game_root = pathlib.Path(entered)
        game_root = _normalize_game_root(game_root)

        property_catalog_tool = (
            options.property_catalog_tool
            or _bundled_tool("KCD2OnlinePropertyCatalog.exe")
        )
        if property_catalog_tool is None:
            raise GameDataError("KCD2OnlinePropertyCatalog.exe is not bundled or specified")
        signature_audit_tool = (
            options.signature_audit_tool
            or _bundled_tool("KCD2OnlineSignatureAudit.exe")
        )
        if signature_audit_tool is not None:
            print("Auditing the installed WHGame.dll...", flush=True)
            _run_signature_audit(game_root, signature_audit_tool)

        output_path = options.output or _default_output()
        print(
            f"Generating dedicated-server game data from {game_root}...",
            flush=True,
        )
        output = generate_server_game_data(
            game_root, output_path, property_catalog_tool
        )
    except GameDataError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        result = 2
    else:
        print(f"Game data generated successfully: {output}")
        result = 0
    if interactive:
        try:
            input("Press Enter to close...")
        except EOFError:
            pass
    return result


if __name__ == "__main__":
    raise SystemExit(main())
