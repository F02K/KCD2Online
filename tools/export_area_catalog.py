#!/usr/bin/env python3
"""Export KCD2 native areas, POIs, workstations, and site relationships to JSON.

The level XML contains the semantic entity data, while ``triggerareas.fubar``
contains the world-space polygon prisms used by TriggerArea entities.  This
tool deliberately keeps native facts separate from derived grouping/tags so
consumers can decide how much inference they want to use.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable
import xml.etree.ElementTree as ET
import zipfile


SCHEMA = "kcd2-area-catalog/v1"
PROPERTY_SCHEMA = "kcd2-property-context/v1"
TREE_SCHEMA = "kcd2-place-tree/v2"
AREA_CLASSES = {
    "AreaBox",
    "AreaShape",
    "AreaSphere",
    "AreaUnion",
    "SequenceArea",
    "SmartAreaShape",
    "TriggerArea",
    "VisibilityArea",
}
FEATURE_CLASSES = {
    "AlchemyTable": "alchemy_table",
    "Blacksmithing": "blacksmithing_station",
    "FoodProcessingTrigger": "food_processing_station",
    "Grindstone": "grindstone",
    "Shop": "shop",
    "Smithery": "smithery",
}
ITEM_CLASSES = {
    "AlchemyTable": "alchemy_table",
    "BedTrigger": "bed",
    "Blacksmithing": "blacksmithing_station",
    "CartStash": "chest",
    "DestroStash": "chest",
    "FireplaceSmartObject": "fireplace",
    "FoodProcessingTrigger": "food_processing",
    "Grindstone": "grindstone",
    "Ladder": "ladder",
    "Shop": "shop",
    "Smithery": "smithery",
    "Stash": "chest",
}
EXCLUDED_AREA_CLASSES = {
    "Birds",
    "CatHolder",
    "Chickens",
    "ChickensBrownLight",
    "ChickensWhite",
    "FieldMouse",
    "Fish",
    "Raptors",
    "Rats",
    "SequenceArea",
    "VisibilityArea",
}
TREE_EXCLUDED_SITE_SEGMENTS = {
    "_common", "_enviro", "_quest", "_script", "area", "areas", "audio",
    "cinematic", "cinematics", "crime", "layer", "layers", "location", "main",
    "mission", "missions", "scheduler", "script", "scripts", "static", "streamed",
    "streamedforbattle",
}
STRUCTURAL_KINDS = {
    "settlement", "district", "compound", "castle", "courtyard", "building", "room",
}
TECHNICAL_SITE_SEGMENTS = {
    "_common",
    "_enviro",
    "_script",
    "area",
    "areas",
    "audio",
    "crime",
    "location",
    "scheduler",
    "static",
    "streamed",
    "streamedforbattle",
}

# These are semantic tags only.  The native source text used to infer them is
# retained in every record, and no inferred value replaces a native field.
KEYWORD_TAGS = {
    "alchemy": ("alchemy", "alchemist"),
    "apothecary": ("apothecary", "pharmacist"),
    "apartment": ("apartment", "flat"),
    "armourer": ("armourer", "armorer"),
    "baker": ("baker", "bakery"),
    "barber": ("barber",),
    "bathhouse": ("bathhouse", "baths", "/spa"),
    "blacksmith": ("blacksmith", "smithery", "smithy"),
    "butcher": ("butcher", "butchery"),
    "castle": ("castle",),
    "city": ("/city", "city_"),
    "compound": ("property", "courtyard", "farmstead", "homestead"),
    "fortress": ("fortress",),
    "fisherman": ("fisherman",),
    "gunsmith": ("gunsmith",),
    "herbalist": ("herbalist",),
    "home": ("_home", "/home", "house_"),
    "horse_trader": ("horsetrader", "horse_trader"),
    "house": ("/houses/", "workerhouse", "house_"),
    "hunter": ("hunter", "huntsman"),
    "inn": ("/inn", "innkeeper", "hostinec", "hotel"),
    "merchant": ("merchant", "generaltrader", "generalmerchant"),
    "mill": ("/mill", "miller"),
    "monastery": ("monastery", "klaster"),
    "palace": ("palace",),
    "priest": ("priest", "parson", "clergy"),
    "room": ("room", "chamber", "/cells", "hall"),
    "saddler": ("saddler",),
    "scribe": ("scribe", "transcribe"),
    "settlement": ("settlement", "/village"),
    "shoemaker": ("shoemaker", "cobbler"),
    "shop": ("shop", "trader", "trafficker"),
    "tailor": ("tailor",),
    "tavern": ("tavern", "hospoda"),
    "weaponsmith": ("weaponsmith", "weapon_smith"),
    "workshop": ("workshop",),
}
REQUESTED_TAGS = {
    "alchemy",
    "apartment",
    "apothecary",
    "armourer",
    "baker",
    "barber",
    "bathhouse",
    "blacksmith",
    "butcher",
    "castle",
    "city",
    "compound",
    "fortress",
    "fisherman",
    "gunsmith",
    "herbalist",
    "home",
    "horse_trader",
    "house",
    "hunter",
    "inn",
    "job",
    "location",
    "merchant",
    "mill",
    "monastery",
    "palace",
    "priest",
    "room",
    "saddler",
    "scribe",
    "settlement",
    "shoemaker",
    "shop",
    "tailor",
    "tavern",
    "weaponsmith",
    "workshop",
}

CRIME_DOOR_KIND = re.compile(r"crime_doorKind\.([a-z_]+)", re.IGNORECASE)
ASSET_NAME = re.compile(r"\[(structures/[^\]:]+)", re.IGNORECASE)
ASSET_GUID_SUFFIX = re.compile(
    r"_[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$",
    re.IGNORECASE,
)
HAND_NAMED_AREA = re.compile(
    r"^(?P<place>k[a-z0-9]+)_(?P<kind>area|court|trespass|tresspass|private)"
    r"(?P<street>[a-z_]*?)(?P<number>[0-9]+)Area_[0-9]+$",
    re.IGNORECASE,
)
NON_HOUSE_GROUP_WORDS = {
    "market", "markt", "trh", "tent", "camp", "mine", "mines", "mining",
    "vineyard", "winery", "wine", "vinice",
}


def parse_vec3(value: str | None, default: list[float] | None = None) -> list[float] | None:
    if not value:
        return default
    try:
        result = [float(component) for component in value.split(",")]
    except ValueError:
        return default
    return result if len(result) == 3 and all(math.isfinite(x) for x in result) else default


def parse_quat(value: str | None) -> list[float]:
    if not value:
        return [1.0, 0.0, 0.0, 0.0]
    try:
        result = [float(component) for component in value.split(",")]
    except ValueError:
        return [1.0, 0.0, 0.0, 0.0]
    if len(result) != 4 or not all(math.isfinite(x) for x in result):
        return [1.0, 0.0, 0.0, 0.0]
    return result


def guid_to_u64(value: str) -> int:
    parts = value.lower().split("-")
    if len(parts) != 3 or tuple(map(len, parts)) != (8, 4, 4):
        raise ValueError(f"not a KCD2 entity GUID: {value}")
    return int(parts[0], 16) | int(parts[1], 16) << 32 | int(parts[2], 16) << 48


def u64_to_guid(value: int) -> str:
    return f"{value & 0xffffffff:08x}-{value >> 32 & 0xffff:04x}-{value >> 48 & 0xffff:04x}"


def guid_to_engine_id(value: str) -> str:
    """Return the 16-character hexadecimal ID used by the game engine."""
    return f"{guid_to_u64(value):016x}"


def round_number(value: float) -> float:
    return round(float(value), 3)


def round_vec3(value: list[float] | None) -> list[float] | None:
    return [round_number(component) for component in value] if value is not None else None


def transform_point(
    local: list[float], position: list[float], rotation: list[float], scale: list[float]
) -> list[float]:
    """Apply CryEngine's WXYZ quaternion transform to one local point."""
    x, y, z = (local[index] * scale[index] for index in range(3))
    w, qx, qy, qz = rotation
    # q * v * conjugate(q), expanded into a 3x3 matrix.
    rx = (1 - 2 * (qy * qy + qz * qz)) * x + 2 * (qx * qy - qz * w) * y + 2 * (qx * qz + qy * w) * z
    ry = 2 * (qx * qy + qz * w) * x + (1 - 2 * (qx * qx + qz * qz)) * y + 2 * (qy * qz - qx * w) * z
    rz = 2 * (qx * qz - qy * w) * x + 2 * (qy * qz + qx * w) * y + (1 - 2 * (qx * qx + qy * qy)) * z
    return [position[0] + rx, position[1] + ry, position[2] + rz]


def polygon_area(points: list[list[float]]) -> float:
    if len(points) < 3:
        return 0.0
    return abs(sum(
        points[index][0] * points[(index + 1) % len(points)][1]
        - points[(index + 1) % len(points)][0] * points[index][1]
        for index in range(len(points))
    )) * 0.5


def geometry_bbox(
    points: list[list[float]],
    height: float = 0.0,
    reference_z: float | None = None,
) -> dict[str, list[float]] | None:
    if not points:
        return None
    base_z = min(point[2] for point in points) if reference_z is None else reference_z
    extruded_z = base_z + height
    point_min_z = min(point[2] for point in points)
    return {
        "min": [min(p[0] for p in points), min(p[1] for p in points), min(point_min_z, base_z, extruded_z)],
        "max": [max(p[0] for p in points), max(p[1] for p in points), max(max(p[2] for p in points), extruded_z)],
    }


def point_in_polygon(point: list[float], polygon: list[list[float]]) -> bool:
    x, y = point[:2]
    inside = False
    previous = len(polygon) - 1
    for current in range(len(polygon)):
        x1, y1 = polygon[current][:2]
        x2, y2 = polygon[previous][:2]
        if (y1 > y) != (y2 > y):
            crossing = (x2 - x1) * (y - y1) / (y2 - y1) + x1
            if x < crossing:
                inside = not inside
        previous = current
    return inside


def point_segment_distance_2d(point: list[float], start: list[float], end: list[float]) -> float:
    px, py = point[:2]
    ax, ay = start[:2]
    bx, by = end[:2]
    dx, dy = bx - ax, by - ay
    length_squared = dx * dx + dy * dy
    if length_squared == 0:
        return math.hypot(px - ax, py - ay)
    factor = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length_squared))
    return math.hypot(px - (ax + factor * dx), py - (ay + factor * dy))


def polygon_distance_2d(point: list[float], polygon: list[list[float]]) -> float:
    if point_in_polygon(point, polygon):
        return 0.0
    return min(
        point_segment_distance_2d(point, polygon[index - 1], polygon[index])
        for index in range(len(polygon))
    ) if polygon else math.inf


def area_geometry_distance(
    geometry: dict[str, Any] | None,
    point: list[float],
    by_guid: dict[str, dict[str, Any]] | None = None,
) -> float:
    """Distance from a point to an area volume, or zero when it is inside."""
    if not geometry:
        return math.inf
    if geometry.get("kind") == "union" and by_guid is not None:
        distances = [
            area_geometry_distance(by_guid[guid].get("geometry"), point, by_guid)
            for guid in geometry.get("member_guids", [])
            if guid in by_guid
        ]
        return min(distances, default=math.inf)
    if geometry.get("kind") == "sphere":
        return max(0.0, math.dist(point, geometry["center_world"]) - geometry["radius"])

    polygon = geometry.get("polygon_world")
    bbox = geometry.get("bounding_box")
    if not polygon or not bbox:
        return math.inf
    horizontal = polygon_distance_2d(point, polygon)
    vertical = max(bbox["min"][2] - point[2], 0.0, point[2] - bbox["max"][2])
    return math.hypot(horizontal, vertical)


def geometry_measure(area: dict[str, Any], by_guid: dict[str, dict[str, Any]]) -> float:
    geometry = area.get("geometry") or {}
    if geometry.get("kind") == "union":
        return sum(
            geometry_measure(by_guid[guid], by_guid)
            for guid in geometry.get("member_guids", [])
            if guid in by_guid
        )
    if geometry.get("area_2d") is not None:
        return float(geometry["area_2d"])
    bbox = geometry.get("bounding_box")
    if not bbox:
        return math.inf
    return max(0.0, bbox["max"][0] - bbox["min"][0]) * max(0.0, bbox["max"][1] - bbox["min"][1])


def parse_triggerareas(data: bytes) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Decode the version-3 triggerareas.fubar polygon-prism stream."""
    if len(data) < 12:
        raise ValueError("triggerareas.fubar is shorter than its header")
    version, header_value, declared_count = struct.unpack_from("<III", data)
    if version != 3:
        raise ValueError(f"unsupported triggerareas.fubar version {version}")
    offset = 12
    records = []
    while offset < len(data):
        if offset + 16 > len(data):
            raise ValueError(f"truncated TriggerArea record at byte {offset}")
        guid_u64, height, point_count = struct.unpack_from("<QfI", data, offset)
        offset += 16
        byte_count = point_count * 12
        if point_count == 0 or offset + byte_count > len(data):
            raise ValueError(f"invalid point count {point_count} at byte {offset - 4}")
        points = [list(struct.unpack_from("<fff", data, offset + index * 12)) for index in range(point_count)]
        offset += byte_count
        if not math.isfinite(height) or not all(math.isfinite(v) for point in points for v in point):
            raise ValueError(f"non-finite TriggerArea geometry for {u64_to_guid(guid_u64)}")
        records.append({
            "guid": u64_to_guid(guid_u64),
            "guid_u64": str(guid_u64),
            "height": height,
            "polygon_world": points,
        })
    if len(records) != declared_count:
        raise ValueError(f"TriggerArea count mismatch: header={declared_count}, decoded={len(records)}")
    return {
        "version": version,
        # The middle header field is retained verbatim. Its semantic meaning is
        # not published, so do not mislabel it as a checksum or hash.
        "header_value_hex": f"{header_value:08x}",
        "record_count": declared_count,
    }, records


def flatten_attributes(node: ET.Element | None) -> dict[str, Any]:
    result: dict[str, Any] = {}
    if node is None:
        return result
    for child in node.iter():
        prefix = ".".join(part.tag for part in _ancestry(node, child))
        for key, value in child.attrib.items():
            name = f"{prefix}.{key}" if prefix else key
            if name not in result:
                result[name] = value
            elif isinstance(result[name], list):
                result[name].append(value)
            else:
                result[name] = [result[name], value]
    return result


def _ancestry(root: ET.Element, wanted: ET.Element) -> list[ET.Element]:
    if root is wanted:
        return []
    stack: list[tuple[ET.Element, list[ET.Element]]] = [(root, [])]
    while stack:
        node, path = stack.pop()
        for child in node:
            if child is wanted:
                return path + [child]
            stack.append((child, path + [child]))
    return []


def normalize_path(value: str) -> str:
    return "/".join(part for part in value.replace("\\", "/").split("/") if part).lower()


def site_path(editor_layer: str) -> str:
    parts = normalize_path(editor_layer).split("/") if editor_layer else []
    while parts and (parts[-1] in TECHNICAL_SITE_SEGMENTS or parts[-1].startswith("_")):
        parts.pop()
    return "/".join(parts)


def site_ancestors(value: str) -> list[str]:
    parts = value.split("/") if value else []
    return ["/".join(parts[:index]) for index in range(1, len(parts) + 1)]


def infer_tags(*values: str) -> set[str]:
    blob = " ".join(value for value in values if value).replace("\\", "/").lower()
    tags = {tag for tag, needles in KEYWORD_TAGS.items() if any(needle in blob for needle in needles)}
    professions = set(re.findall(r"profession(?:s)?/([a-z0-9_]+)", blob))
    if professions:
        tags.add("job")
        tags.update(f"profession:{profession}" for profession in professions)
    return tags


def tags_from_site_path(value: str) -> set[str]:
    """Infer area tags from this area's own normalized path segments only."""
    return infer_tags(*(segment for segment in value.split("/") if segment))


def asset_from_name(value: str) -> str | None:
    match = ASSET_NAME.search(value)
    if not match:
        return None
    return ASSET_GUID_SUFFIX.sub("", match.group(1).replace("\\", "/"))


def address_from_site_paths(paths: Iterable[str]) -> tuple[str | None, int | None, str | None]:
    """Parse only the explicitly supported city/village district address form."""
    matches: set[tuple[str, int, str]] = set()
    for path in paths:
        segments = normalize_path(path).split("/")
        for index, segment in enumerate(segments[:-2]):
            if segment not in {"city", "village"}:
                continue
            district = segments[index + 1]
            numbered = re.fullmatch(r"([0-9]+)_([a-z0-9_]+)", segments[index + 2])
            if numbered:
                matches.add((district, int(numbered.group(1)), numbered.group(2)))
    if len(matches) != 1:
        return None, None, None
    return next(iter(matches))


def crime_door_kind(link_name: str) -> str | None:
    if not link_name.lower().startswith("crime_door"):
        return None
    match = CRIME_DOOR_KIND.search(link_name)
    return match.group(1).lower() if match else None


def stable_generated_utc(build_info: dict[str, Any]) -> str:
    """Use immutable source metadata so identical inputs create identical bytes."""
    value = str(build_info.get("assembly", {}).get("DateTestedData", ""))
    if value:
        return value
    return "1970-01-01T00:00:00+00:00"


def labels_from_properties(properties: dict[str, Any]) -> list[str]:
    value = properties.get("Label", "")
    if not isinstance(value, str):
        return []
    return sorted({label.strip() for label in value.split(";") if label.strip()})


def localized_strings(game_root: Path) -> dict[str, dict[str, str]]:
    result: dict[str, dict[str, str]] = {}
    path = game_root / "Localization" / "German_xml.pak"
    if not path.is_file():
        return result
    with zipfile.ZipFile(path) as archive:
        for name in archive.namelist():
            if not name.lower().endswith(".xml"):
                continue
            root = ET.fromstring(archive.read(name))
            for row in root.iter("Row"):
                cells = [(cell.text or "") for cell in row.findall("Cell")]
                if len(cells) >= 3 and cells[0]:
                    result[cells[0].casefold()] = {"key": cells[0], "en": cells[1], "de": cells[2]}
    return result


def resolve_label(key: str, strings: dict[str, dict[str, str]]) -> dict[str, str]:
    normalized = key.removeprefix("@")
    return strings.get(normalized.casefold(), {"key": normalized, "en": "", "de": ""})


def game_build_info(game_root: Path) -> dict[str, Any]:
    path = game_root / "whdlversions.json"
    if not path.is_file():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    wanted = {"GameData", "Trosecko LE", "Kutnohorsko LE", "Klaster LE", "Localization XML"}
    versions = {
        row["Name"]: row.get("SelectedVersion", {}).get("versionId", "")
        for row in data.get("Configurations", [])
        if row.get("Name") in wanted
    }
    return {
        "assembly": data.get("Assembly", {}),
        "preset": data.get("Preset", {}).get("Name", ""),
        "versions": versions,
    }


def table_rows(archive: zipfile.ZipFile, name: str, tag: str, id_field: str) -> dict[str, dict[str, str]]:
    root = ET.fromstring(archive.read(name))
    return {node.attrib[id_field].lower(): dict(node.attrib) for node in root.iter(tag) if id_field in node.attrib}


def read_zip_entry(archive: zipfile.ZipFile, name: str) -> bytes:
    """Read KCD2 ZIP entries whose local headers use Windows separators.

    Python 3.13 rejects a central-directory ``layers/foo.xml`` name when the
    corresponding local header spells it ``layers\\foo.xml``.  The game PAK is
    valid and libzip accepts it, so normalize only the comparison name on the
    temporary ZipInfo object.
    """
    info = archive.getinfo(name)
    original = info.orig_filename
    try:
        info.orig_filename = original.replace("/", "\\")
        return archive.read(info)
    finally:
        info.orig_filename = original


def geometry_from_xml(entity: ET.Element) -> dict[str, Any] | None:
    area = entity.find("Area")
    if area is None:
        return None
    position = parse_vec3(entity.attrib.get("Pos"), [0.0, 0.0, 0.0]) or [0.0, 0.0, 0.0]
    rotation = parse_quat(entity.attrib.get("Rotate"))
    scale = parse_vec3(entity.attrib.get("Scale"), [1.0, 1.0, 1.0]) or [1.0, 1.0, 1.0]
    points = [
        point
        for point in (parse_vec3(node.attrib.get("Pos")) for node in area.findall("./Points/Point"))
        if point is not None
    ]
    common = {"native_area_attributes": dict(area.attrib)}
    if points:
        world = [transform_point(point, position, rotation, scale) for point in points]
        height = float(area.attrib.get("Height", "0") or 0) * scale[2]
        return {
            **common,
            "kind": "polygon_prism",
            "coordinate_space": "world",
            "height": height,
            "polygon_local": points,
            "polygon_world": world,
            "area_2d": polygon_area(world),
            "bounding_box": geometry_bbox(world, height, position[2]),
        }
    box_min = parse_vec3(area.attrib.get("BoxMin"))
    box_max = parse_vec3(area.attrib.get("BoxMax"))
    if box_min and box_max:
        local = [
            [box_min[0], box_min[1], box_min[2]], [box_max[0], box_min[1], box_min[2]],
            [box_max[0], box_max[1], box_min[2]], [box_min[0], box_max[1], box_min[2]],
        ]
        world = [transform_point(point, position, rotation, scale) for point in local]
        top = [transform_point([p[0], p[1], box_max[2]], position, rotation, scale) for p in local]
        all_points = world + top
        return {
            **common,
            "kind": "oriented_box",
            "coordinate_space": "world",
            "box_local": {"min": box_min, "max": box_max},
            "polygon_world": world,
            "corners_world": all_points,
            "area_2d": polygon_area(world),
            "bounding_box": geometry_bbox(all_points),
        }
    sphere_center = parse_vec3(area.attrib.get("SphereCenter"))
    if sphere_center is not None and area.attrib.get("SphereRadius"):
        center = transform_point(sphere_center, position, rotation, scale)
        radius = float(area.attrib["SphereRadius"]) * max(abs(v) for v in scale)
        return {
            **common,
            "kind": "sphere",
            "coordinate_space": "world",
            "center_world": center,
            "radius": radius,
            "bounding_box": {
                "min": [v - radius for v in center],
                "max": [v + radius for v in center],
            },
        }
    return {**common, "kind": "unresolved_native_area"}


def merge_entity(existing: dict[str, Any], incoming: dict[str, Any]) -> None:
    existing["source_xmls"] = sorted(set(existing["source_xmls"] + incoming["source_xmls"]))
    for key in ("name", "entity_class", "editor_layer", "position", "rotation", "scale", "geometry"):
        if not existing.get(key) and incoming.get(key):
            existing[key] = incoming[key]
    existing["properties"].update({key: value for key, value in incoming["properties"].items() if key not in existing["properties"]})
    known_links = {(link.get("name"), link.get("target_guid"), link.get("target_id")) for link in existing["links"]}
    existing["links"].extend(link for link in incoming["links"] if (link.get("name"), link.get("target_guid"), link.get("target_id")) not in known_links)


def entity_record(entity: ET.Element, source: str, local_guids: dict[str, str]) -> dict[str, Any]:
    guid = entity.attrib["EntityGuid"].lower()
    links = []
    for link in entity.findall("./EntityLinks/Link"):
        target_id = link.attrib.get("TargetId", "")
        target_guid = link.attrib.get("TargetGuid", "").lower()
        if not target_guid or target_guid == "00000000-0000-0000":
            target_guid = local_guids.get(target_id, "")
        links.append({
            "name": link.attrib.get("Name", ""),
            "target_guid": target_guid,
            "target_id": target_id,
            "target_engine_id": guid_to_engine_id(target_guid) if target_guid else None,
        })
    properties = flatten_attributes(entity.find("Properties"))
    editor_layer = entity.attrib.get("EditorLayer", "")
    return {
        "guid": guid,
        "guid_u64": str(guid_to_u64(guid)),
        "engine_id": guid_to_engine_id(guid),
        "entity_id": entity.attrib.get("EntityId", ""),
        "name": entity.attrib.get("Name", ""),
        "entity_class": entity.attrib.get("EntityClass", ""),
        "editor_layer": editor_layer,
        "site_path": site_path(editor_layer),
        "position": parse_vec3(entity.attrib.get("Pos")),
        "rotation": parse_quat(entity.attrib.get("Rotate")),
        "scale": parse_vec3(entity.attrib.get("Scale"), [1.0, 1.0, 1.0]),
        "labels": labels_from_properties(properties),
        "properties": properties,
        "links": links,
        "geometry": geometry_from_xml(entity),
        "source_xmls": [source],
    }


def parse_document(
    data: bytes,
    source: str,
    areas: dict[str, dict[str, Any]],
    features: dict[str, dict[str, Any]],
    doors: dict[str, dict[str, Any]],
    items: dict[str, dict[str, Any]],
    pois: list[dict[str, Any]],
    site_evidence: dict[str, dict[str, set[str]]],
) -> None:
    root = ET.fromstring(data)
    entities = [node for node in root if node.tag == "Entity" and node.attrib.get("EntityGuid")]
    local_guids = {node.attrib.get("EntityId", ""): node.attrib["EntityGuid"].lower() for node in entities}
    for entity in entities:
        editor_layer = entity.attrib.get("EditorLayer", "")
        path = site_path(editor_layer)
        entity_class = entity.attrib.get("EntityClass", "")
        name = entity.attrib.get("Name", "")
        xml_text = ET.tostring(entity, encoding="unicode")
        tags = infer_tags(path, name, entity_class, xml_text)
        if path and tags:
            site_evidence[path]["tags"].update(tags)
            site_evidence[path]["entity_guids"].add(entity.attrib["EntityGuid"].lower())
        if entity_class == "AnimDoor":
            guid = entity.attrib["EntityGuid"].lower()
            properties = flatten_attributes(entity.find("Properties"))
            doors[guid] = {
                "guid": guid,
                "engine_id": guid_to_engine_id(guid),
                "name": name,
                "entity_id": entity.attrib.get("EntityId", ""),
                "position": parse_vec3(entity.attrib.get("Pos")),
                "site_path": path,
                "properties": properties,
                "source_xmls": [source],
            }
        is_area = entity_class in AREA_CLASSES or entity.find("Area") is not None
        feature_kind = FEATURE_CLASSES.get(entity_class)
        item_kind = ITEM_CLASSES.get(entity_class)
        if not feature_kind and any(word in f" {entity_class.lower()} {name.lower()} " for word in ("workbench", "anvil", "grindstone")):
            feature_kind = "workstation"
        if not is_area and not feature_kind and not item_kind:
            continue
        record = entity_record(entity, source, local_guids)
        if is_area:
            if record["guid"] in areas:
                merge_entity(areas[record["guid"]], record)
            else:
                areas[record["guid"]] = record
        if feature_kind:
            record["feature_kind"] = feature_kind
            record["semantic_tags"] = sorted(tags | ({"job"} if feature_kind in {"shop", "smithery", "blacksmithing_station"} else set()))
            if record["guid"] in features:
                merge_entity(features[record["guid"]], record)
            else:
                features[record["guid"]] = record
        if item_kind:
            item_record = dict(record)
            item_record["item_kind"] = item_kind
            if item_record["guid"] in items:
                merge_entity(items[item_record["guid"]], item_record)
            else:
                items[item_record["guid"]] = item_record
    for index, node in enumerate(root):
        if node.tag != "Object" or node.attrib.get("Type") != "POI":
            continue
        attrs = dict(node.attrib)
        stable = "\0".join((source, str(index), attrs.get("Name", ""), attrs.get("Pos", "")))
        pois.append({
            "poi_id": hashlib.sha256(stable.encode()).hexdigest()[:24],
            "name": attrs.get("Name", ""),
            "position": parse_vec3(attrs.get("Pos")),
            "poi_type_id": attrs.get("POITypeId", "").lower(),
            "location_id": attrs.get("LocationId", "").lower(),
            "custom_label_key": attrs.get("CustomLabel", "").removeprefix("@"),
            "custom_mark_type": attrs.get("CustomMarkType", ""),
            "is_discoverable": attrs.get("IsDiscoverable") == "1",
            "is_fast_travel": attrs.get("IsFastTravel") == "1",
            "discovery_distance": float(attrs.get("DiscoveryDistance", "0") or 0),
            "source_xml": source,
            "native_attributes": attrs,
        })


def geometry_contains(geometry: dict[str, Any] | None, point: list[float]) -> bool:
    if not geometry:
        return False
    bbox = geometry.get("bounding_box")
    if bbox and not all(bbox["min"][axis] - 1.0 <= point[axis] <= bbox["max"][axis] + 1.0 for axis in range(3)):
        return False
    polygon = geometry.get("polygon_world")
    if polygon:
        return point_in_polygon(point, polygon)
    if geometry.get("kind") == "sphere":
        return math.dist(point, geometry["center_world"]) <= geometry["radius"] + 1.0
    return False


def containment(area_values: Iterable[dict[str, Any]], point: list[float] | None, limit: int | None = 32) -> list[str]:
    if point is None:
        return []
    matches = []
    for area in area_values:
        geometry = area.get("geometry")
        if geometry_contains(geometry, point):
            bbox = geometry.get("bounding_box") or {"min": [0, 0, 0], "max": [1e9, 1e9, 1e9]}
            size = math.prod(max(0.001, bbox["max"][axis] - bbox["min"][axis]) for axis in range(3))
            matches.append((size, area["guid"]))
    ordered = [guid for _, guid in sorted(matches)]
    return ordered[:limit] if limit is not None else ordered


def aggregate_union_geometry(area: dict[str, Any], by_guid: dict[str, dict[str, Any]]) -> None:
    members = area["relations"]["union_member_guids"]
    boxes = [by_guid[guid].get("geometry", {}).get("bounding_box") for guid in members if guid in by_guid]
    boxes = [box for box in boxes if box]
    area["geometry"] = {
        "kind": "union",
        "coordinate_space": "world",
        "member_guids": members,
        "bounding_box": {
            "min": [min(box["min"][axis] for box in boxes) for axis in range(3)],
            "max": [max(box["max"][axis] for box in boxes) for axis in range(3)],
        } if boxes else None,
    }


def union_root_guid(guid: str, areas: dict[str, dict[str, Any]]) -> str:
    """Promote a union member to one deterministic outermost AreaUnion."""
    seen = {guid}
    current = guid
    while current in areas:
        parents = [
            parent for parent in areas[current]["relations"]["member_of_union_guids"]
            if parent not in seen and asset_from_name(areas[parent]["name"])
        ]
        if not parents:
            return current
        current = min(parents, key=lambda value: areas[value]["engine_id"])
        seen.add(current)
    return guid


def union_area_guids(guid: str, areas: dict[str, dict[str, Any]]) -> list[str]:
    result: list[str] = []

    def visit(current: str) -> None:
        if current in result or current not in areas:
            return
        result.append(current)
        for member in sorted(
            areas[current]["relations"]["union_member_guids"],
            key=lambda value: areas[value]["engine_id"] if value in areas else value,
        ):
            visit(member)

    visit(guid)
    return result


def combined_bounds(area_guids: Iterable[str], areas: dict[str, dict[str, Any]]) -> dict[str, list[float]] | None:
    boxes = [
        areas[guid].get("geometry", {}).get("bounding_box")
        for guid in area_guids
        if guid in areas and areas[guid].get("geometry")
    ]
    boxes = [box for box in boxes if box]
    if not boxes:
        return None
    return {
        "min": [round_number(min(box["min"][axis] for box in boxes)) for axis in range(3)],
        "max": [round_number(max(box["max"][axis] for box in boxes)) for axis in range(3)],
    }


def area_height(area: dict[str, Any]) -> float | None:
    geometry = area.get("geometry") or {}
    if geometry.get("height") is not None:
        return abs(float(geometry["height"]))
    bbox = geometry.get("bounding_box")
    if bbox:
        return abs(float(bbox["max"][2]) - float(bbox["min"][2]))
    return None


def floors_hint(height: float | None, doors: list[dict[str, Any]]) -> int | None:
    levels: list[float] = []
    for door in sorted((door for door in doors if door.get("position")), key=lambda value: value["position"][2]):
        z = float(door["position"][2])
        if not levels or z - levels[-1] > 1.5:
            levels.append(z)
    if len(levels) > 1:
        return len(levels)
    if height is not None and 1.8 <= height <= 24.0:
        return max(1, round(height / 3.2))
    return None


def build_property_catalog(
    level_name: str,
    level_id: str,
    build_info: dict[str, Any],
    areas: dict[str, dict[str, Any]],
    doors: dict[str, dict[str, Any]],
    features: dict[str, dict[str, Any]],
    locations: list[dict[str, Any]],
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Build mutually exclusive building/property contexts in the requested stages."""
    properties: list[dict[str, Any]] = []
    assigned_areas: dict[str, dict[str, Any]] = {}
    assigned_doors: dict[str, dict[str, Any]] = {}

    def door_row(guid: str, kind: str | None) -> dict[str, Any]:
        door = doors.get(guid, {})
        return {
            "door_id": guid_to_engine_id(guid),
            "kind": kind,
            "position": round_vec3(door.get("position")),
        }

    def make_property(
        main_guid: str,
        area_guids: list[str],
        property_doors: list[dict[str, Any]],
        origin: str,
        confidence: str,
        representative_guid: str | None = None,
    ) -> dict[str, Any]:
        unique_guids = [main_guid] + sorted(
            {guid for guid in area_guids if guid != main_guid},
            key=lambda value: areas[value]["engine_id"],
        )
        main = areas[main_guid]
        representative = areas.get(representative_guid or "", main)
        paths = [areas[guid]["site_path"] for guid in unique_guids]
        district, number, trade = address_from_site_paths(paths)
        labels = sorted({label for guid in unique_guids for label in areas[guid]["labels"]})
        asset = next((asset_from_name(areas[guid]["name"]) for guid in unique_guids if asset_from_name(areas[guid]["name"])), None)
        height = area_height(main)
        floor_area = geometry_measure(main, areas)
        if not math.isfinite(floor_area):
            floor_area = 0.0
        property_row = {
            "area_id": main["engine_id"],
            "area_ids": [areas[guid]["engine_id"] for guid in unique_guids],
            "name": main["name"],
            "asset": asset,
            "labels": labels,
            "settlement": None,
            "district": district,
            "number": number,
            "trade_layer": trade,
            "site_path": representative["site_path"] or main["site_path"],
            "doors": sorted(property_doors, key=lambda value: (value["door_id"], value["kind"] or "")),
            "features": [],
            "floor": {
                "area_2d": round_number(floor_area),
                "height": round_number(height) if height is not None else None,
                "floors_hint": floors_hint(height, property_doors),
            },
            "bounds": combined_bounds(unique_guids, areas),
            "origin": origin,
            "confidence": confidence,
            "_area_guids": unique_guids,
        }
        properties.append(property_row)
        for guid in unique_guids:
            assigned_areas[guid] = property_row
        for row in property_doors:
            assigned_doors[row["door_id"]] = property_row
        return property_row

    # Stage A: native crime_door links are exact ownership evidence.
    exact_groups: dict[tuple[Any, ...], dict[str, Any]] = {}
    for area in sorted(areas.values(), key=lambda value: value["engine_id"]):
        linked = [(link, crime_door_kind(link["name"])) for link in area["links"]]
        linked = [(link, kind) for link, kind in linked if kind and link["target_guid"]]
        if not linked:
            continue
        main_guid = union_root_guid(area["guid"], areas)
        door_signature = tuple(sorted(link["target_engine_id"] for link, _ in linked))
        asset = asset_from_name(area["name"])
        if main_guid != area["guid"]:
            group_key: tuple[Any, ...] = ("union", main_guid)
        elif asset:
            # Some building prefabs expose multiple linked component areas but
            # repeat the exact same physical doors on each component.
            group_key = ("asset_doors", asset, door_signature)
        else:
            group_key = ("area", area["guid"])
        group = exact_groups.setdefault(group_key, {"main_guid": main_guid, "sources": set(), "doors": {}})
        group["sources"].add(area["guid"])
        for link, kind in linked:
            engine_id = link["target_engine_id"]
            group["doors"].setdefault(engine_id, door_row(link["target_guid"], kind))
    # Native data can repeat one door set on several component/court areas.
    # Connected components by door ID are one property, preserving exclusivity.
    raw_exact_groups = list(exact_groups.values())
    parents = list(range(len(raw_exact_groups)))

    def find(index: int) -> int:
        while parents[index] != index:
            parents[index] = parents[parents[index]]
            index = parents[index]
        return index

    def join(left: int, right: int) -> None:
        left_root, right_root = find(left), find(right)
        if left_root != right_root:
            parents[right_root] = left_root

    door_group: dict[str, int] = {}
    for index, group in enumerate(raw_exact_groups):
        for door_id in group["doors"]:
            if door_id in door_group:
                join(index, door_group[door_id])
            else:
                door_group[door_id] = index
    merged_exact_groups: dict[int, dict[str, Any]] = {}
    for index, group in enumerate(raw_exact_groups):
        merged = merged_exact_groups.setdefault(
            find(index), {"main_guids": set(), "sources": set(), "doors": {}}
        )
        merged["main_guids"].add(group["main_guid"])
        merged["sources"].update(group["sources"])
        merged["doors"].update(group["doors"])
    ordered_exact_groups = sorted(
        merged_exact_groups.values(),
        key=lambda value: min(areas[guid]["engine_id"] for guid in value["sources"]),
    )
    for group in ordered_exact_groups:
        union_mains = [
            guid for guid in group["main_guids"]
            if areas[guid]["entity_class"] == "AreaUnion" and asset_from_name(areas[guid]["name"])
        ]
        main_guid = min(
            union_mains or list(group["sources"]),
            key=lambda value: areas[value]["engine_id"],
        )
        member_guids = union_area_guids(main_guid, areas)
        for source_guid in group["sources"]:
            if source_guid not in member_guids:
                member_guids.append(source_guid)
        make_property(
            main_guid,
            member_guids,
            list(group["doors"].values()),
            "door_link",
            "exact",
            min(group["sources"], key=lambda value: areas[value]["engine_id"]),
        )

    def nearby_doors(main_guid: str) -> list[tuple[float, str]]:
        position_rows = []
        geometry = areas[main_guid].get("geometry")
        for guid, door in doors.items():
            if door.get("position") is None:
                continue
            distance = area_geometry_distance(geometry, door["position"], areas)
            if distance <= 2.0 + 1e-6:
                position_rows.append((distance, guid))
        return sorted(position_rows, key=lambda value: (value[0], doors[value[1]]["engine_id"]))

    def unique_nearby(rows: Iterable[tuple[float, str]]) -> list[tuple[float, str]]:
        closest: dict[str, float] = {}
        for distance, guid in rows:
            closest[guid] = min(distance, closest.get(guid, math.inf))
        return sorted(
            ((distance, guid) for guid, distance in closest.items()),
            key=lambda value: (value[0], doors[value[1]]["engine_id"]),
        )

    # Stage B: asset-backed private areas, smallest footprint first.
    asset_candidates: dict[str, str] = {}
    for area in areas.values():
        if not asset_from_name(area["name"]) or "private" not in {label.lower() for label in area["labels"]}:
            continue
        if any(crime_door_kind(link["name"]) for link in area["links"]):
            continue
        main_guid = union_root_guid(area["guid"], areas)
        previous = asset_candidates.get(main_guid)
        if previous is None or geometry_measure(area, areas) < geometry_measure(areas[previous], areas):
            asset_candidates[main_guid] = area["guid"]
    ordered_assets = sorted(
        asset_candidates.items(),
        key=lambda item: (geometry_measure(areas[item[0]], areas), areas[item[0]]["engine_id"]),
    )
    for main_guid, representative_guid in ordered_assets:
        candidate_guids = union_area_guids(main_guid, areas)
        if any(guid in assigned_areas for guid in candidate_guids):
            continue
        nearby = nearby_doors(main_guid)
        free = [(distance, guid) for distance, guid in nearby if doors[guid]["engine_id"] not in assigned_doors]
        if not free and nearby:
            owner = assigned_doors.get(doors[nearby[0][1]]["engine_id"])
            if owner is not None:
                additions = [guid for guid in candidate_guids if guid not in assigned_areas]
                for guid in additions:
                    owner["_area_guids"].append(guid)
                    owner["area_ids"].append(areas[guid]["engine_id"])
                    assigned_areas[guid] = owner
                owner["area_ids"] = [owner["area_id"]] + sorted(set(owner["area_ids"]) - {owner["area_id"]})
                owner["bounds"] = combined_bounds(owner["_area_guids"], areas)
                continue
        property_doors = [door_row(guid, None) for _, guid in free]
        make_property(main_guid, candidate_guids, property_doors, "asset_name", "spatial", representative_guid)

    # Stage C: explicitly named location/street/number zones.
    named_groups: dict[tuple[str, str, int], list[tuple[str, re.Match[str]]]] = defaultdict(list)
    for area in areas.values():
        if area["guid"] in assigned_areas:
            continue
        match = HAND_NAMED_AREA.fullmatch(area["name"])
        if match:
            key = (match.group("place").lower(), match.group("street").lower(), int(match.group("number")))
            named_groups[key].append((area["guid"], match))
    for key in sorted(named_groups):
        rows = named_groups[key]
        raw_guids = [guid for guid, _ in rows]
        text = " ".join(areas[guid]["name"] + " " + areas[guid]["site_path"] for guid in raw_guids).lower()
        has_house_word = any(word in text for word in ("home", "house", "dwelling"))
        if any(word in text for word in NON_HOUSE_GROUP_WORDS) and not has_house_word:
            continue
        if all(match.group("kind").lower() == "court" for _, match in rows) and not has_house_word:
            # A court becomes a zone of a real house below when it contains an
            # already assigned door; otherwise it is deliberately omitted.
            pass
        main_guid = min(raw_guids, key=lambda value: (geometry_measure(areas[value], areas), areas[value]["engine_id"]))
        nearby = []
        for guid in raw_guids:
            nearby.extend((distance, door_guid) for distance, door_guid in nearby_doors(guid))
        nearby = unique_nearby(nearby)
        occupied = [(distance, guid) for distance, guid in nearby if doors[guid]["engine_id"] in assigned_doors]
        if occupied:
            owner = assigned_doors[doors[occupied[0][1]]["engine_id"]]
            for guid in raw_guids:
                if guid not in assigned_areas:
                    owner["_area_guids"].append(guid)
                    owner["area_ids"].append(areas[guid]["engine_id"])
                    assigned_areas[guid] = owner
            owner["area_ids"] = [owner["area_id"]] + sorted(set(owner["area_ids"]) - {owner["area_id"]})
            owner["bounds"] = combined_bounds(owner["_area_guids"], areas)
            continue
        if all(match.group("kind").lower() == "court" for _, match in rows) and not has_house_word:
            continue
        free = [(distance, guid) for distance, guid in nearby if doors[guid]["engine_id"] not in assigned_doors]
        property_doors = [door_row(guid, None) for _, guid in free]
        make_property(main_guid, raw_guids, property_doors, "hand_named_group", "named", main_guid)

    location_by_guid = {row["area_guid"]: row for row in locations}
    for property_row in properties:
        location_guids = {
            location_guid
            for guid in property_row["_area_guids"]
            for location_guid in areas[guid]["relations"]["containing_location_area_guids"]
            if location_guid in location_by_guid
        }
        if location_guids:
            selected = min(
                location_guids,
                key=lambda value: (geometry_measure(areas[value], areas), areas[value]["engine_id"]),
            )
            location = location_by_guid[selected]
            property_row["settlement"] = {
                "location_id": location["location_id"],
                "name": {
                    "de": location["display_label"].get("de", ""),
                    "en": location["display_label"].get("en", ""),
                },
            }

    # Assign each feature to at most one property, based only on property zones.
    for feature in sorted(features.values(), key=lambda value: value["engine_id"]):
        if feature.get("position") is None:
            continue
        candidates = []
        for property_row in properties:
            distances = [
                area_geometry_distance(areas[guid].get("geometry"), feature["position"], areas)
                for guid in property_row["_area_guids"]
            ]
            distance = min(distances, default=math.inf)
            if distance <= 2.0 + 1e-6:
                candidates.append((distance, property_row["floor"]["area_2d"], property_row["area_id"], property_row))
        if not candidates:
            continue
        distance, _, _, owner = min(candidates, key=lambda value: value[:3])
        owner["features"].append({
            "kind": feature["feature_kind"],
            "guid_engine": feature["engine_id"],
            "position": round_vec3(feature["position"]),
            "inside": distance <= 1.0 + 1e-6,
        })

    for property_row in properties:
        property_row["features"].sort(key=lambda value: (value["guid_engine"], value["kind"]))
        property_row.pop("_area_guids")
    properties.sort(key=lambda value: value["area_id"])
    by_origin = {
        origin: sum(row["origin"] == origin for row in properties)
        for origin in ("door_link", "asset_name", "hand_named_group")
    }
    counts = {
        "properties": len(properties),
        "by_origin": by_origin,
        "with_settlement": sum(row["settlement"] is not None for row in properties),
        "with_district_and_number": sum(row["district"] is not None and row["number"] is not None for row in properties),
        "with_trade_layer": sum(row["trade_layer"] is not None for row in properties),
        "with_features": sum(bool(row["features"]) for row in properties),
    }
    payload = {
        "schema": PROPERTY_SCHEMA,
        "source": {"game_build": build_info},
        "level": {"id": level_id, "directory": level_name},
        "counts": counts,
        "properties": properties,
    }
    return payload, counts


TRADE_TAGS = {
    "alchemy": "alchemy",
    "apothecary": "alchemy",
    "blacksmith": "smithing",
    "weaponsmith": "smithing",
    "armourer": "smithing",
    "baker": "baker",
    "butcher": "butcher",
    "tailor": "tailor",
    "shoemaker": "shoemaker",
    "merchant": "merchant",
    "shop": "shop",
    "inn": "inn",
    "tavern": "tavern",
    "herbalist": "herbalist",
}


def area_is_excluded_from_tree(area: dict[str, Any]) -> bool:
    if area["entity_class"] in EXCLUDED_AREA_CLASSES:
        return True
    path = normalize_path(area.get("editor_layer", ""))
    name = area.get("name", "").lower()
    return name.startswith("cin_") or "/cinematics/" in f"/{path}/"


def access_from_labels(labels: Iterable[str]) -> str | None:
    lowered = {label.lower() for label in labels}
    for value in ("prohibited", "private", "personal", "semipersonal", "semipublic", "public"):
        if value in lowered:
            return value
    return None


def tree_site_kind(segment: str) -> tuple[str, str] | None:
    lowered = segment.lower()
    if re.search(r"(?:^|[_-])(castle|fortress|tvrz|hrad|palace)(?:$|[_-])", lowered):
        return "castle", "site_tree"
    if "courtyard" in lowered or lowered.startswith("court") or "court" in lowered:
        return "courtyard", "site_tree"
    if re.fullmatch(r"[0-9]+_[a-z0-9_]+", lowered) or any(
        word in lowered for word in ("house", "home", "inn", "tavern", "smith", "shop", "bakery")
    ):
        return "building", "site_tree"
    return None


def area_kind(area: dict[str, Any], is_location: bool) -> tuple[str, str]:
    labels = {label.lower() for label in area["labels"]}
    text = f"{area['name']} {area['site_path']} {asset_from_name(area['name']) or ''}".lower()
    local_text = f"{area['name']} {asset_from_name(area['name']) or ''}".lower()
    if is_location and "crimedistrict" in labels and "settlement" not in labels:
        return "district", "location"
    if is_location:
        return "settlement", "location"
    if "crimedistrict" in labels and "settlement" not in labels:
        return "district", "location"
    entity_name = area["name"].split("[", 1)[0].lower()
    castle_name = re.sub(r"^[a-z]{4}_", "", entity_name)
    castle_name = re.sub(r"_(?:sh|area(?:_\d+)?)$", "", castle_name)
    if castle_name.startswith("court") or any(word in castle_name for word in ("courtyard", "_court", "yard")):
        return "courtyard", "asset"
    if re.fullmatch(r"(?:castle|fortress|tvrz|hrad)", castle_name):
        return "castle", "asset"
    if "castle" in labels and not any(word in castle_name for word in ("room", "interior", "attic", "cellar")):
        return "castle", "label"
    if any(word in castle_name for word in ("room", "interior", "attic", "cellar", "bath", "armory", "arcade", "chapel")):
        return "room", "asset"
    if re.search(r"(?:^|_)(?:castle|fortress|tvrz|hrad)(?:_|$)", castle_name):
        return "building", "asset"
    if area["entity_class"] == "AreaUnion":
        return ("building", "asset") if asset_from_name(area["name"]) else ("compound", "union_member")
    if area["relations"]["member_of_union_guids"]:
        if re.search(r"(?:^|[_/])(floor|storey|story)[0-9_]*(?:$|[/_])", text):
            return "floor", "union_member"
        return "room", "union_member"
    if "courtyard" in local_text or re.search(r"(?:^|[/_])court", local_text):
        return "courtyard", "site_tree"
    if asset_from_name(area["name"]) or any(crime_door_kind(link["name"]) for link in area["links"]):
        return "building", "asset" if asset_from_name(area["name"]) else "label"
    if "interior" in labels:
        return "room", "label"
    if any(word in text for word in ("field", "meadow", "market", "camp", "vineyard", "forest", "outside", "outdoor")):
        return "outdoor", "site_tree"
    return "other", "label" if labels else "site_tree"


def tree_overlay_kind(area: dict[str, Any]) -> tuple[str, str] | None:
    """Return the non-place overlay kind and the exact matching rule.

    Only the entity name before an optional asset suffix is used for the roof
    rule.  Asset names such as ``small_roof_to_ground_house`` describe a real
    building and must not turn that building into an overlay.
    """
    entity_name = (area.get("name") or "").split("[", 1)[0]
    name = entity_name.lower()
    path = normalize_path(area.get("site_path", "")).lower()
    labels = {label.lower() for label in area.get("labels", [])}
    combined = f"{name} {path}"
    rules = (
        ("roof", "name:*_roof*", "_roof" in name or "roofzone" in name),
        ("birds", "name:birdsTakeoff*", "birdstakeoff" in name),
        ("stealth", "name:stealthArea*", "stealtharea" in name),
        ("music", "name/path:*music*", "music" in combined),
        ("deterrent", "name/label:*deterrent*", "deterrent" in combined or "deterrentarea" in labels),
        ("ladder", "name/path:*ladder*", "ladder" in combined),
        ("dog_forbidden", "name/path:*dogForbidden*", "dogforbidden" in combined or "dogforbiden" in combined),
        ("horse", "name:*horse*", "horse" in name),
        ("trigger", "name:trigger[*", name.startswith("trigger[") or name.startswith("triggerarea[")),
        ("trigger", "name:*TriggerArea (no asset)", "triggerarea" in name and asset_from_name(area.get("name", "")) is None),
        ("other", "name:sa_*", name.startswith("sa_")),
    )
    return next(((kind, source) for kind, source, matches in rules if matches), None)


def area_center(area: dict[str, Any]) -> list[float] | None:
    geometry = area.get("geometry") or {}
    bbox = geometry.get("bounding_box")
    if bbox:
        return [(bbox["min"][axis] + bbox["max"][axis]) / 2 for axis in range(3)]
    return area.get("position")


def structural_node_kind(kind: str) -> bool:
    return kind in STRUCTURAL_KINDS


def trade_from_text(*values: str) -> tuple[str | None, str | None]:
    tags = infer_tags(*values)
    for tag, trade in TRADE_TAGS.items():
        if tag in tags:
            return trade, "site_layer"
    return None, None


def build_spatial_grid(
    rows: Iterable[tuple[str, dict[str, Any] | None]],
    cell_size: float = 64.0,
) -> dict[tuple[int, int], set[str]]:
    grid: dict[tuple[int, int], set[str]] = defaultdict(set)
    for key, geometry in rows:
        bbox = (geometry or {}).get("bounding_box")
        if not bbox:
            continue
        min_x, min_y = (math.floor(bbox["min"][axis] / cell_size) for axis in (0, 1))
        max_x, max_y = (math.floor(bbox["max"][axis] / cell_size) for axis in (0, 1))
        if (max_x - min_x + 1) * (max_y - min_y + 1) > 4096:
            continue
        for x in range(min_x, max_x + 1):
            for y in range(min_y, max_y + 1):
                grid[(x, y)].add(key)
    return grid


def grid_candidates(
    grid: dict[tuple[int, int], set[str]],
    point: list[float],
    margin: float = 0.0,
    cell_size: float = 64.0,
) -> set[str]:
    radius = max(0, math.ceil(margin / cell_size))
    cell_x, cell_y = math.floor(point[0] / cell_size), math.floor(point[1] / cell_size)
    return {
        key
        for x in range(cell_x - radius, cell_x + radius + 1)
        for y in range(cell_y - radius, cell_y + radius + 1)
        for key in grid.get((x, y), set())
    }


def tree_floor_info(asset: str | None) -> dict[str, Any]:
    if asset:
        match = re.search(r"(?:^|_)([1-9])s(?:_|$)", asset.rpartition("/")[2], re.IGNORECASE)
        if match:
            return {"count": int(match.group(1)), "source": "prefab"}
    return {"count": None, "source": None}


def door_lock_data(door: dict[str, Any]) -> dict[str, Any]:
    properties = door.get("properties", {})
    difficulty: float | int | None = None
    key_item: str | None = None
    for key, value in properties.items():
        lowered = key.lower()
        scalar = value[0] if isinstance(value, list) and value else value
        if difficulty is None and "difficulty" in lowered:
            try:
                number = float(scalar)
                difficulty = int(number) if number.is_integer() else number
            except (TypeError, ValueError):
                pass
        if key_item is None and "key" in lowered and ("guid" in lowered or "item" in lowered):
            key_item = str(scalar) or None
    has_lock_data = any("lock" in key.lower() for key in properties)
    return {
        "difficulty": difficulty,
        "key_item": key_item,
        "source": f"{door['source_xmls'][0]}/Properties.Lock" if has_lock_data else None,
    }


def build_place_tree_v1(
    level_name: str,
    level_id: str,
    build_info: dict[str, Any],
    areas: dict[str, dict[str, Any]],
    doors: dict[str, dict[str, Any]],
    items: dict[str, dict[str, Any]],
    locations: list[dict[str, Any]],
) -> tuple[dict[str, Any], dict[str, Any]]:
    included_areas = {guid: area for guid, area in areas.items() if not area_is_excluded_from_tree(area)}
    excluded_counts: dict[str, int] = defaultdict(int)
    for area in areas.values():
        if area_is_excluded_from_tree(area):
            excluded_counts[area["entity_class"]] += 1

    level_node_id = f"n:level:{level_id}"
    nodes: dict[str, dict[str, Any]] = {
        level_node_id: {
            "id": level_node_id,
            "engine_id": None,
            "kind": "level",
            "kind_source": "site_tree",
            "name": {"de": None, "en": None, "raw": level_name},
            "parent": None,
            "parent_source": None,
            "area_ids": [],
            "labels": [],
            "access": None,
            "trade": None,
            "trade_source": None,
            "asset": None,
            "geometry": None,
            "floors": {"count": None, "source": None},
            "doors": [],
            "contents": [],
            "children": [],
        }
    }
    area_node: dict[str, str] = {}
    location_by_area = {row["area_guid"]: row for row in locations}
    for guid, area in sorted(included_areas.items(), key=lambda value: value[1]["engine_id"]):
        node_id = f"n:{area['engine_id']}"
        location = location_by_area.get(guid)
        kind, kind_source = area_kind(area, location is not None)
        asset = asset_from_name(area["name"])
        trade, trade_source = trade_from_text(area["site_path"], asset or "")
        geometry = area.get("geometry") or {}
        display = location.get("display_label", {}) if location else {}
        node = {
            "id": node_id,
            "engine_id": area["engine_id"],
            "kind": kind,
            "kind_source": kind_source,
            "name": {
                "de": display.get("de") or None,
                "en": display.get("en") or None,
                "raw": area["name"] or None,
            },
            "parent": None,
            "parent_source": None,
            "area_ids": [area["engine_id"]],
            "labels": area["labels"],
            "access": access_from_labels(area["labels"]),
            "trade": trade,
            "trade_source": trade_source,
            "asset": asset,
            "geometry": {
                "area_2d": round_number(geometry["area_2d"]) if geometry.get("area_2d") is not None else None,
                "height": round_number(area_height(area)) if area_height(area) is not None else None,
                "bounds": combined_bounds([guid], areas),
                "polygon_ref": area["engine_id"] if geometry.get("polygon_world") else None,
            } if geometry else None,
            "floors": tree_floor_info(asset),
            "doors": [],
            "contents": [],
            "children": [],
            "_area_guid": guid,
            "_area_guids": [guid],
            "_site_path": area["site_path"],
        }
        nodes[node_id] = node
        area_node[guid] = node_id

    # Create only site path levels that describe a physical place.
    excluded_segments: dict[str, int] = defaultdict(int)
    site_nodes: dict[str, str] = {}
    site_descendant_areas: dict[str, set[str]] = defaultdict(set)
    for guid, area in included_areas.items():
        parts = area["site_path"].split("/") if area["site_path"] else []
        for index, segment in enumerate(parts):
            if segment.lower() in TREE_EXCLUDED_SITE_SEGMENTS or segment.startswith("_"):
                excluded_segments[segment.lower()] += 1
                continue
            descriptor = tree_site_kind(segment)
            if not descriptor:
                continue
            path = "/".join(parts[: index + 1])
            site_descendant_areas[path].add(guid)
            if path in site_nodes:
                continue
            stable = hashlib.sha256(f"{level_id}\0{path}".encode()).hexdigest()[:20]
            node_id = f"n:site:{stable}"
            kind, kind_source = descriptor
            trade, trade_source = trade_from_text(segment)
            site_nodes[path] = node_id
            nodes[node_id] = {
                "id": node_id,
                "engine_id": None,
                "kind": kind,
                "kind_source": kind_source,
                "name": {"de": None, "en": None, "raw": segment},
                "parent": None,
                "parent_source": None,
                "area_ids": [],
                "labels": [],
                "access": None,
                "trade": trade,
                "trade_source": trade_source,
                "asset": None,
                "geometry": None,
                "floors": {"count": None, "source": None},
                "doors": [],
                "contents": [],
                "children": [],
                "_site_path": path,
            }

    def containing_location_node(guid: str) -> str | None:
        candidates = [
            location_guid
            for location_guid in included_areas[guid]["relations"]["containing_location_area_guids"]
            if location_guid in area_node
        ]
        if not candidates:
            return None
        selected = min(candidates, key=lambda value: (geometry_measure(areas[value], areas), areas[value]["engine_id"]))
        return area_node[selected]

    # Parent virtual sites by native site ancestry, then by native location.
    for path, node_id in sorted(site_nodes.items()):
        ancestors = [candidate for candidate in site_nodes if path.startswith(candidate + "/")]
        if ancestors:
            nodes[node_id]["parent"] = site_nodes[max(ancestors, key=len)]
            nodes[node_id]["parent_source"] = "site_tree"
            continue
        descendants = sorted(site_descendant_areas[path], key=lambda guid: areas[guid]["engine_id"])
        location_nodes = [containing_location_node(guid) for guid in descendants]
        location_nodes = [value for value in location_nodes if value]
        nodes[node_id]["parent"] = location_nodes[0] if location_nodes else level_node_id
        nodes[node_id]["parent_source"] = "location" if location_nodes else "site_tree"

    # Apply native parent rules to area nodes.
    pending_geometry: list[str] = []
    for guid, node_id in area_node.items():
        area = areas[guid]
        node = nodes[node_id]
        if node["kind"] == "settlement":
            node["parent"] = level_node_id
            node["parent_source"] = "location"
            continue
        if node["kind"] == "district":
            node["parent"] = containing_location_node(guid) or level_node_id
            node["parent_source"] = "location"
            continue
        union_parents = [parent for parent in area["relations"]["member_of_union_guids"] if parent in area_node]
        if union_parents:
            selected = min(union_parents, key=lambda value: areas[value]["engine_id"])
            node["parent"] = area_node[selected]
            node["parent_source"] = "union_member"
            continue
        site_candidates = [path for path in site_nodes if area["site_path"] == path or area["site_path"].startswith(path + "/")]
        if site_candidates:
            node["parent"] = site_nodes[max(site_candidates, key=len)]
            node["parent_source"] = "site_tree"
            continue
        location_node = containing_location_node(guid)
        if location_node:
            node["parent"] = location_node
            node["parent_source"] = "location"
            continue
        pending_geometry.append(guid)

    area_grid = build_spatial_grid((guid, area.get("geometry")) for guid, area in included_areas.items())
    for guid in pending_geometry:
        area = areas[guid]
        node = nodes[area_node[guid]]
        geometry = area.get("geometry") or {}
        bbox = geometry.get("bounding_box")
        selected_parent = None
        if bbox:
            center = [(bbox["min"][axis] + bbox["max"][axis]) / 2 for axis in range(3)]
            own_measure = geometry_measure(area, areas)
            own_locations = set(area["relations"]["containing_location_area_guids"])
            candidates = []
            for candidate_guid in grid_candidates(area_grid, center):
                if candidate_guid == guid:
                    continue
                candidate = areas[candidate_guid]
                candidate_locations = set(candidate["relations"]["containing_location_area_guids"])
                if own_locations != candidate_locations:
                    continue
                measure = geometry_measure(candidate, areas)
                if measure <= own_measure or area_geometry_distance(candidate.get("geometry"), center, areas) > 1e-6:
                    continue
                candidates.append((measure, candidate["engine_id"], candidate_guid))
            if candidates:
                selected_parent = area_node[min(candidates)[2]]
        if selected_parent:
            node["parent"] = selected_parent
            node["parent_source"] = "geometry"
        else:
            node["parent"] = level_node_id
            node["parent_source"] = "label" if any(
                label.lower() in {"castle", "compound", "fortress"} for label in area["labels"]
            ) else "site_tree"

    # Compact technical sibling areas into one node while retaining every
    # engine area ID exactly once. Spatially meaningful and union nodes remain
    # individual so the place hierarchy is not flattened.
    merge_groups: dict[tuple[Any, ...], list[str]] = defaultdict(list)
    for node_id, node in nodes.items():
        if node.get("_area_guid") is None or node["kind"] not in {"other", "outdoor"}:
            continue
        merge_groups[(
            node["parent"], node["parent_source"], node["kind"], node["access"],
            node["trade"], node.get("_site_path", ""), tuple(node["labels"]),
        )].append(node_id)
    replacement: dict[str, str] = {}
    for key, member_node_ids in sorted(merge_groups.items(), key=lambda value: repr(value[0])):
        if len(member_node_ids) < 2:
            continue
        member_node_ids.sort(key=lambda value: nodes[value]["engine_id"] or "")
        main = nodes[member_node_ids[0]]
        stable = hashlib.sha256(
            (f"{level_id}\0group\0" + "\0".join(nodes[value]["engine_id"] or "" for value in member_node_ids)).encode()
        ).hexdigest()[:20]
        group_id = f"n:group:{stable}"
        raw_names = {nodes[value]["name"]["raw"] for value in member_node_ids}
        area_guids = [nodes[value]["_area_guid"] for value in member_node_ids]
        group_node = dict(main)
        group_node.update({
            "id": group_id,
            "engine_id": main["engine_id"],
            "name": {
                "de": None,
                "en": None,
                "raw": next(iter(raw_names)) if len(raw_names) == 1 else (main.get("_site_path", "").rpartition("/")[2] or None),
            },
            "area_ids": [areas[guid]["engine_id"] for guid in area_guids],
            "labels": sorted({label for value in member_node_ids for label in nodes[value]["labels"]}),
            "geometry": main["geometry"],
            "doors": [],
            "contents": [],
            "children": [],
            "_area_guid": area_guids[0],
            "_area_guids": area_guids,
        })
        nodes[group_id] = group_node
        for old_id, guid in zip(member_node_ids, area_guids):
            replacement[old_id] = group_id
            area_node[guid] = group_id
            del nodes[old_id]
    if replacement:
        for node in nodes.values():
            while node.get("parent") in replacement:
                node["parent"] = replacement[node["parent"]]

    # Geometry matching uses only real area nodes; virtual sites never claim points.
    node_measure = {
        node_id: min(geometry_measure(areas[guid], areas) for guid in node["_area_guids"])
        for node_id, node in nodes.items()
        if node.get("_area_guids")
    }

    def node_distance(node_id: str, point: list[float]) -> float:
        return min(
            area_geometry_distance(areas[guid].get("geometry"), point, areas)
            for guid in nodes[node_id]["_area_guids"]
        )

    def point_node_candidates(point: list[float], maximum_distance: float) -> list[tuple[float, float, str, str]]:
        best_by_node: dict[str, tuple[float, float, str, str]] = {}
        for guid in grid_candidates(area_grid, point, maximum_distance):
            node_id = area_node[guid]
            distance = area_geometry_distance(areas[guid].get("geometry"), point, areas)
            if distance <= maximum_distance + 1e-6:
                row = (distance, node_measure[node_id], node_id, guid)
                if node_id not in best_by_node or row < best_by_node[node_id]:
                    best_by_node[node_id] = row
        return sorted(best_by_node.values())

    linked_doors: dict[str, list[tuple[str, str]]] = defaultdict(list)
    linked_items: dict[str, list[tuple[str, str]]] = defaultdict(list)
    item_by_entity_id = {item["entity_id"]: guid for guid, item in items.items() if item.get("entity_id")}
    for guid, area in included_areas.items():
        for link in area["links"]:
            target_guid = link["target_guid"]
            kind = crime_door_kind(link["name"])
            if kind and target_guid in doors:
                linked_doors[target_guid].append((guid, kind))
            if not target_guid:
                target_guid = item_by_entity_id.get(link["target_id"], "")
            if target_guid in items:
                linked_items[target_guid].append((guid, link["name"]))

    door_rows = []
    for guid, door in sorted(doors.items(), key=lambda value: value[1]["engine_id"]):
        links = linked_doors.get(guid, [])
        node_id = None
        node_source = None
        reason = None
        candidates_out: list[str] = []
        kind = None
        if links:
            kinds = sorted({link_kind for _, link_kind in links})
            kind = kinds[0] if len(kinds) == 1 else None
            linked_candidates = sorted(
                {area_node[area_guid] for area_guid, _ in links if area_guid in area_node},
                key=lambda value: (node_measure.get(value, math.inf), value),
            )
            if linked_candidates:
                node_id = linked_candidates[0]
                node_source = "crime_door_link"
                if len(kinds) > 1:
                    reason = "ambiguous"
                    candidates_out = linked_candidates
        elif door.get("position") is None:
            reason = "no_area_link"
        else:
            spatial = point_node_candidates(door["position"], 1.5)
            if spatial:
                best = spatial[0]
                tied = [row for row in spatial if abs(row[0] - best[0]) < 1e-6 and abs(row[1] - best[1]) < 1e-6]
                if len(tied) > 1:
                    reason = "ambiguous"
                    candidates_out = sorted({row[2] for row in tied})
                else:
                    node_id = best[2]
                    node_source = "union_member" if nodes[node_id]["parent_source"] == "union_member" else "geometry"
            else:
                reason = "outside_all_nodes"
        row = {
            "id": f"door:{door['engine_id']}",
            "engine_id": door["engine_id"],
            "name": door["name"],
            "position": round_vec3(door.get("position")),
            "kind": kind,
            "kind_source": "crime_door_link" if kind else None,
            "node": node_id,
            "node_source": node_source,
            "opens_to": None,
            "linked_area_engine_ids": sorted(areas[area_guid]["engine_id"] for area_guid, _ in links),
            "lock": door_lock_data(door),
            "reason": reason,
            "candidates": candidates_out,
        }
        door_rows.append(row)
        if node_id:
            nodes[node_id]["doors"].append(row["id"])

    entity_classes = {
        **{guid: area["entity_class"] for guid, area in areas.items()},
        **{guid: "AnimDoor" for guid in doors},
        **{guid: item["entity_class"] for guid, item in items.items()},
    }
    evidence = []
    item_rows = []
    for guid, item in sorted(items.items(), key=lambda value: value[1]["engine_id"]):
        links = linked_items.get(guid, [])
        node_id = None
        node_source = None
        inside = False
        if links:
            candidates = sorted(
                {area_node[area_guid] for area_guid, _ in links if area_guid in area_node},
                key=lambda value: (node_measure.get(value, math.inf), value),
            )
            if candidates:
                node_id = candidates[0]
                node_source = "link"
                inside = item.get("position") is not None and node_distance(node_id, item["position"]) <= 1.0 + 1e-6
                source_area, link_name = links[0]
                evidence.append({
                    "a": node_id,
                    "b": f"item:{item['engine_id']}",
                    "relation": "belongs_to_district" if "crime_stolenitemsstoragechest" in link_name.lower() else "same_building",
                    "via": "crime_link" if "crime_" in link_name.lower() else "home_equipment",
                    "source": f"{areas[source_area]['source_xmls'][0]}/{link_name}",
                    "used_for": "item.node",
                })
        if node_id is None and item.get("position") is not None:
            spatial = point_node_candidates(item["position"], 2.0)
            if spatial:
                node_id = spatial[0][2]
                node_source = "union_member" if nodes[node_id]["parent_source"] == "union_member" else "geometry"
                inside = spatial[0][0] <= 1.0 + 1e-6
        item_links = []
        for link in item["links"]:
            target_guid = link["target_guid"]
            item_links.append({
                "name": link["name"],
                "to_engine_id": guid_to_engine_id(target_guid) if target_guid else None,
                "to_class": entity_classes.get(target_guid),
            })
        row = {
            "id": f"item:{item['engine_id']}",
            "engine_id": item["engine_id"],
            "class": item["entity_class"],
            "kind": item["item_kind"],
            "name": item["name"],
            "position": round_vec3(item.get("position")),
            "node": node_id,
            "node_source": node_source,
            "inside": inside,
            "links": sorted(item_links, key=lambda value: (value["name"], value["to_engine_id"] or "")),
        }
        item_rows.append(row)
        if node_id:
            nodes[node_id]["contents"].append(row["id"])
            if nodes[node_id]["trade"] is None:
                feature_trade = {
                    "alchemy_table": "alchemy",
                    "blacksmithing_station": "smithing",
                    "smithery": "smithing",
                    "grindstone": "smithing",
                    "shop": "shop",
                }.get(row["kind"])
                if feature_trade:
                    nodes[node_id]["trade"] = feature_trade
                    nodes[node_id]["trade_source"] = "feature"

    for node in nodes.values():
        node["doors"].sort()
        node["contents"].sort()
        if node["parent"]:
            nodes[node["parent"]]["children"].append(node["id"])
    for node in nodes.values():
        node["children"].sort()
        node.pop("_area_guid", None)
        node.pop("_area_guids", None)
        node.pop("_site_path", None)

    node_rows = sorted(nodes.values(), key=lambda value: value["id"])
    door_rows.sort(key=lambda value: value["id"])
    item_rows.sort(key=lambda value: value["id"])
    evidence.sort(key=lambda value: (value["a"], value["b"], value["relation"], value["via"]))
    nodes_by_kind = {kind: sum(node["kind"] == kind for node in node_rows) for kind in (
        "level", "settlement", "district", "compound", "castle", "courtyard",
        "building", "floor", "room", "outdoor", "other",
    )}
    parent_sources = {source: sum(node["parent_source"] == source for node in node_rows) for source in (
        "site_tree", "union_member", "location", "geometry", "label",
    )}
    door_reasons = {
        reason: sum(door["node"] is None and door["reason"] == reason for door in door_rows)
        for reason in ("no_area_link", "outside_all_nodes", "ambiguous")
    }
    item_kinds = {
        kind: sum(item["kind"] == kind for item in item_rows)
        for kind in sorted({item["kind"] for item in item_rows})
    }
    counts = {
        "nodes": len(node_rows),
        "nodes_by_kind": nodes_by_kind,
        "nodes_by_parent_source": parent_sources,
        "nodes_without_parent": sum(node["kind"] != "level" and node["parent"] is None for node in node_rows),
        "doors": len(door_rows),
        "doors_with_node": sum(door["node"] is not None for door in door_rows),
        "doors_without_node": sum(door["node"] is None for door in door_rows),
        "doors_without_node_by_reason": door_reasons,
        "items": len(item_rows),
        "items_by_kind": item_kinds,
        "items_with_node": sum(item["node"] is not None for item in item_rows),
        "areas_excluded_by_class": dict(sorted(excluded_counts.items())),
        "areas_without_node": len(included_areas) - len(area_node),
        "excluded_site_segments": dict(sorted(excluded_segments.items())),
    }
    unresolved_doors = [door for door in door_rows if door["node"] is None]
    unresolved_items = [item for item in item_rows if item["node"] is None]
    payload = {
        "schema": TREE_SCHEMA,
        "source": {"game_build": build_info},
        "level": {"id": level_id, "directory": level_name},
        "counts": counts,
        "limitations": {
            "doors_without_node_examples": [door["id"] for door in unresolved_doors[:10]],
            "items_without_node_examples": [item["id"] for item in unresolved_items[:10]],
            "missing_item_classes": [],
        },
        "nodes": node_rows,
        "doors": door_rows,
        "items": item_rows,
        "evidence": evidence,
    }
    return payload, counts


def build_place_tree(
    level_name: str,
    level_id: str,
    build_info: dict[str, Any],
    areas: dict[str, dict[str, Any]],
    doors: dict[str, dict[str, Any]],
    items: dict[str, dict[str, Any]],
    locations: list[dict[str, Any]],
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Build the v2 place tree from physical places, with overlays kept as metadata."""
    source_areas = {guid: area for guid, area in areas.items() if not area_is_excluded_from_tree(area)}
    excluded_counts: dict[str, int] = defaultdict(int)
    for area in areas.values():
        if area_is_excluded_from_tree(area):
            excluded_counts[area["entity_class"]] += 1

    overlay_info = {
        guid: descriptor
        for guid, area in source_areas.items()
        if (descriptor := tree_overlay_kind(area)) is not None
    }
    place_areas = {guid: area for guid, area in source_areas.items() if guid not in overlay_info}
    level_node_id = f"n:level:{level_id}"

    def empty_floor_info(asset: str | None) -> dict[str, Any]:
        hint = tree_floor_info(asset)
        if hint["count"] is not None:
            return {**hint, "reason": None}
        return {"count": None, "source": None, "reason": "no_prefab_floor_count"}

    def base_node(
        node_id: str,
        kind: str,
        kind_source: str,
        raw_name: str | None,
        parent: str | None = None,
    ) -> dict[str, Any]:
        return {
            "id": node_id,
            "engine_id": None,
            "kind": kind,
            "kind_source": kind_source,
            "name": {"de": None, "en": None, "raw": raw_name},
            "parent": parent,
            "parent_source": None,
            "area_ids": [],
            "labels": [],
            "access": None,
            "trade": None,
            "trade_source": None,
            "asset": None,
            "geometry": None,
            "floors": {"count": None, "source": None, "reason": "not_a_prefab_building"},
            "doors": [],
            "contents": [],
            "overlays": [],
            "own": {"doors": 0, "items_by_kind": {}},
            "totals": {"doors": 0, "items_by_kind": {}, "rooms": 0},
            "children": [],
        }

    nodes: dict[str, dict[str, Any]] = {
        level_node_id: base_node(level_node_id, "level", "site_tree", level_name)
    }
    unresolved_node_id = f"n:unresolved:{level_id}"
    unresolved_node = base_node(
        unresolved_node_id, "compound", "unresolved_container", "unresolved_native_context", level_node_id
    )
    unresolved_node["parent_source"] = "unresolved"
    nodes[unresolved_node_id] = unresolved_node
    location_by_area = {row["area_guid"]: row for row in locations}
    area_node: dict[str, str] = {}
    for guid, area in sorted(place_areas.items(), key=lambda value: value[1]["engine_id"]):
        node_id = f"n:{area['engine_id']}"
        location = location_by_area.get(guid)
        kind, kind_source = area_kind(area, location is not None)
        asset = asset_from_name(area["name"])
        trade, trade_source = trade_from_text(area["site_path"], asset or "")
        geometry = area.get("geometry") or {}
        display = location.get("display_label", {}) if location else {}
        node = base_node(node_id, kind, kind_source, area["name"] or None)
        node.update({
            "engine_id": area["engine_id"],
            "name": {
                "de": display.get("de") or None,
                "en": display.get("en") or None,
                "raw": area["name"] or None,
            },
            "area_ids": [area["engine_id"]],
            "labels": area["labels"],
            "access": access_from_labels(area["labels"]),
            "trade": trade,
            "trade_source": trade_source,
            "asset": asset,
            "geometry": {
                "area_2d": round_number(geometry["area_2d"]) if geometry.get("area_2d") is not None else None,
                "height": round_number(area_height(area)) if area_height(area) is not None else None,
                "bounds": combined_bounds([guid], areas),
                "polygon_ref": area["engine_id"] if geometry.get("polygon_world") else None,
            } if geometry else None,
            "floors": empty_floor_info(asset) if kind == "building" else {
                "count": None, "source": None, "reason": "not_a_prefab_building",
            },
            "_area_guids": [guid],
            "_site_path": area["site_path"],
        })
        nodes[node_id] = node
        area_node[guid] = node_id

    excluded_segments: dict[str, int] = defaultdict(int)
    site_nodes: dict[str, str] = {}
    site_descendant_areas: dict[str, set[str]] = defaultdict(set)
    for guid, area in place_areas.items():
        parts = area["site_path"].split("/") if area["site_path"] else []
        for index, segment in enumerate(parts):
            if segment.lower() in TREE_EXCLUDED_SITE_SEGMENTS or segment.startswith("_"):
                excluded_segments[segment.lower()] += 1
                continue
            descriptor = tree_site_kind(segment)
            if not descriptor:
                continue
            path = "/".join(parts[: index + 1])
            site_descendant_areas[path].add(guid)
            if path in site_nodes:
                continue
            stable = hashlib.sha256(f"{level_id}\0{path}".encode()).hexdigest()[:20]
            node_id = f"n:site:{stable}"
            kind, kind_source = descriptor
            trade, trade_source = trade_from_text(segment)
            node = base_node(node_id, kind, kind_source, segment)
            node.update({"trade": trade, "trade_source": trade_source, "_site_path": path})
            nodes[node_id] = node
            site_nodes[path] = node_id

    def containing_location_node(guid: str) -> str | None:
        candidates = [
            location_guid
            for location_guid in areas[guid]["relations"]["containing_location_area_guids"]
            if location_guid in area_node
        ]
        if not candidates:
            return None
        selected = min(candidates, key=lambda value: (geometry_measure(areas[value], areas), areas[value]["engine_id"]))
        return area_node[selected]

    for path, node_id in sorted(site_nodes.items()):
        ancestors = [candidate for candidate in site_nodes if path.startswith(candidate + "/")]
        if ancestors:
            nodes[node_id]["parent"] = site_nodes[max(ancestors, key=len)]
            nodes[node_id]["parent_source"] = "site_tree"
            continue
        descendants = sorted(site_descendant_areas[path], key=lambda guid: areas[guid]["engine_id"])
        location_nodes = [containing_location_node(guid) for guid in descendants]
        location_nodes = [value for value in location_nodes if value]
        nodes[node_id]["parent"] = location_nodes[0] if location_nodes else level_node_id
        nodes[node_id]["parent_source"] = "location" if location_nodes else "site_tree"

    place_grid = build_spatial_grid((guid, area.get("geometry")) for guid, area in place_areas.items())

    def geometric_parent(guid: str) -> str | None:
        area = areas[guid]
        center = area_center(area)
        if center is None:
            return None
        own_measure = geometry_measure(area, areas)
        candidates: list[tuple[float, str, str]] = []
        for candidate_guid in grid_candidates(place_grid, center):
            if candidate_guid == guid:
                continue
            candidate_node = area_node[candidate_guid]
            if not structural_node_kind(nodes[candidate_node]["kind"]):
                continue
            measure = geometry_measure(areas[candidate_guid], areas)
            if measure <= own_measure + 1e-6:
                continue
            if area_geometry_distance(areas[candidate_guid].get("geometry"), center, areas) <= 1e-6:
                candidates.append((measure, areas[candidate_guid]["engine_id"], candidate_node))
        return min(candidates)[2] if candidates else None

    # Required precedence: union, meaningful site path, geometry, location.
    for guid, node_id in sorted(area_node.items(), key=lambda value: areas[value[0]]["engine_id"]):
        area, node = areas[guid], nodes[node_id]
        if node["kind"] == "settlement":
            node["parent"], node["parent_source"] = level_node_id, "location"
            continue
        if node["kind"] == "district":
            location_node = containing_location_node(guid)
            node["parent"] = location_node if location_node and location_node != node_id else level_node_id
            node["parent_source"] = "location"
            continue
        union_parents = [parent for parent in area["relations"]["member_of_union_guids"] if parent in area_node]
        if union_parents:
            selected = min(union_parents, key=lambda value: areas[value]["engine_id"])
            node["parent"], node["parent_source"] = area_node[selected], "union_member"
            continue
        site_candidates = [
            path for path in site_nodes
            if area["site_path"] == path or area["site_path"].startswith(path + "/")
        ]
        if site_candidates:
            node["parent"], node["parent_source"] = site_nodes[max(site_candidates, key=len)], "site_tree"
            continue
        geometry_node = geometric_parent(guid)
        if geometry_node:
            node["parent"], node["parent_source"] = geometry_node, "geometry"
            continue
        location_node = containing_location_node(guid)
        node["parent"] = location_node or level_node_id
        node["parent_source"] = "location" if location_node else "site_tree"

    # Merge a physical castle outline with the virtual site node describing the same place.
    def normalized_castle_name(value: str | None) -> str:
        value = (value or "").split("[", 1)[0].lower()
        value = re.sub(r"^[a-z]{4}_", "", value)
        value = re.sub(r"_(?:sh|area(?:_\d+)?)$", "", value)
        return re.sub(r"[^a-z0-9]+", "_", value).strip("_")

    castle_merges: list[dict[str, Any]] = []
    castle_replacement: dict[str, str] = {}
    for path, site_id in sorted(list(site_nodes.items())):
        if site_id not in nodes or nodes[site_id]["kind"] != "castle":
            continue
        site_key = normalized_castle_name(nodes[site_id]["name"]["raw"])
        candidates = []
        for node_id, node in nodes.items():
            if node_id == site_id or node["kind"] != "castle" or not node.get("_area_guids"):
                continue
            if normalized_castle_name(node["name"]["raw"]) != site_key:
                continue
            physical_path = node.get("_site_path", "")
            if not physical_path or not path.startswith(physical_path.rstrip("/") + "/"):
                continue
            candidates.append((geometry_measure(areas[node["_area_guids"][0]], areas), node_id))
        if not candidates:
            continue
        _, physical_id = max(candidates)
        physical = nodes[physical_id]
        before_site = sum(node.get("parent") == site_id for node in nodes.values())
        before_area = sum(node.get("parent") == physical_id for node in nodes.values())
        site_parent = nodes[site_id]["parent"]
        if physical["parent"] == site_id or physical["parent_source"] == "location":
            physical["parent"] = site_parent
            physical["parent_source"] = "site_tree"
        physical["kind_source"] = f"{physical['kind_source']}+site_tree"
        physical["_site_path"] = path
        for node in nodes.values():
            if node.get("parent") == site_id and node["id"] != physical_id:
                node["parent"] = physical_id
        for site_path, mapped in list(site_nodes.items()):
            if mapped == site_id:
                site_nodes[site_path] = physical_id
        castle_replacement[site_id] = physical_id
        del nodes[site_id]
        after = sum(node.get("parent") == physical_id for node in nodes.values())
        castle_merges.append({
            "name": nodes[physical_id]["name"]["raw"],
            "site_path": path,
            "children_before": {"area": before_area, "site": before_site},
            "children_after": after,
        })
    if castle_replacement:
        for node in nodes.values():
            while node.get("parent") in castle_replacement:
                node["parent"] = castle_replacement[node["parent"]]

    # Compact non-place technical siblings without allowing them to own gameplay entities.
    merge_groups: dict[tuple[Any, ...], list[str]] = defaultdict(list)
    for node_id, node in nodes.items():
        if not node.get("_area_guids") or not (
            node["kind"] in {"other", "outdoor"}
            or (node["kind"] == "compound" and node["kind_source"] == "physical_area")
        ):
            continue
        merge_groups[(
            node["parent"], node["parent_source"], node["kind"], node["access"],
            node["trade"], node.get("_site_path", ""), tuple(node["labels"]),
        )].append(node_id)
    replacement: dict[str, str] = {}
    for key, member_ids in sorted(merge_groups.items(), key=lambda value: repr(value[0])):
        if len(member_ids) < 2:
            continue
        member_ids.sort(key=lambda value: nodes[value]["engine_id"] or "")
        main = nodes[member_ids[0]]
        area_guids = [guid for value in member_ids for guid in nodes[value]["_area_guids"]]
        stable = hashlib.sha256(
            (f"{level_id}\0group\0" + "\0".join(areas[guid]["engine_id"] for guid in area_guids)).encode()
        ).hexdigest()[:20]
        group_id = f"n:group:{stable}"
        group = dict(main)
        raw_names = {nodes[value]["name"]["raw"] for value in member_ids}
        group.update({
            "id": group_id,
            "name": {"de": None, "en": None, "raw": next(iter(raw_names)) if len(raw_names) == 1 else None},
            "area_ids": [areas[guid]["engine_id"] for guid in area_guids],
            "labels": sorted({label for value in member_ids for label in nodes[value]["labels"]}),
            "doors": [], "contents": [], "overlays": [], "children": [],
            "_area_guids": area_guids,
        })
        nodes[group_id] = group
        for old_id in member_ids:
            for guid in nodes[old_id]["_area_guids"]:
                area_node[guid] = group_id
            replacement[old_id] = group_id
            del nodes[old_id]
    if replacement:
        for node in nodes.values():
            while node.get("parent") in replacement:
                node["parent"] = replacement[node["parent"]]

    def structural_ancestor(node_id: str | None) -> str | None:
        seen = set()
        while node_id in nodes and node_id not in seen:
            seen.add(node_id)
            if structural_node_kind(nodes[node_id]["kind"]):
                return node_id
            node_id = nodes[node_id].get("parent")
        return None

    node_measure = {
        node_id: min(geometry_measure(areas[guid], areas) for guid in node["_area_guids"])
        for node_id, node in nodes.items() if node.get("_area_guids")
    }

    def node_distance(node_id: str, point: list[float]) -> float:
        area_guids = nodes[node_id].get("_area_guids", [])
        if not area_guids:
            return math.inf
        return min(
            area_geometry_distance(areas[guid].get("geometry"), point, areas)
            for guid in area_guids
        )

    def structural_point_candidates(point: list[float], maximum_distance: float) -> list[tuple[float, float, str, str]]:
        best: dict[str, tuple[float, float, str, str]] = {}
        for guid in grid_candidates(place_grid, point, maximum_distance):
            source_node = area_node[guid]
            node_id = structural_ancestor(source_node)
            if node_id is None:
                continue
            # A virtual site building may have no polygon of its own.  The
            # contained source area is still native geometric evidence that
            # the point belongs to that structural ancestor.
            distance = area_geometry_distance(areas[guid].get("geometry"), point, areas)
            if distance > maximum_distance + 1e-6:
                continue
            measure = node_measure.get(node_id, geometry_measure(areas[guid], areas))
            row = (distance, measure, node_id, guid)
            if node_id not in best or row < best[node_id]:
                best[node_id] = row
        return sorted(best.values())

    # Attach every overlay to a structural owner; it never becomes a tree node.
    overlay_grid = build_spatial_grid((guid, source_areas[guid].get("geometry")) for guid in overlay_info)
    overlay_owner: dict[str, str] = {}
    unresolved_overlays: list[str] = []
    overlay_pattern_counts: dict[tuple[str, str], int] = defaultdict(int)
    for guid, (kind, rule) in sorted(overlay_info.items(), key=lambda value: source_areas[value[0]]["engine_id"]):
        area = source_areas[guid]
        overlay_pattern_counts[(kind, rule)] += 1
        owner = None
        site_candidates = [
            path for path, node_id in site_nodes.items()
            if (area["site_path"] == path or area["site_path"].startswith(path + "/"))
            and structural_ancestor(node_id)
        ]
        if site_candidates:
            owner = structural_ancestor(site_nodes[max(site_candidates, key=len)])
        center = area_center(area)
        if owner is None and center is not None:
            spatial = structural_point_candidates(center, 0.0)
            owner = spatial[0][2] if spatial else None
        if owner is None:
            location_candidates = [
                location_guid for location_guid in area["relations"]["containing_location_area_guids"]
                if location_guid in area_node
            ]
            if location_candidates:
                owner = structural_ancestor(area_node[min(
                    location_candidates,
                    key=lambda value: (geometry_measure(areas[value], areas), areas[value]["engine_id"]),
                )])
        overlay = {
            "engine_id": area["engine_id"],
            "class": area["entity_class"],
            "name": area["name"],
            "kind": kind,
            "source": rule,
        }
        if owner:
            overlay_owner[guid] = owner
            nodes[owner]["overlays"].append(overlay)
        else:
            unresolved_overlays.append(area["engine_id"])
            overlay_owner[guid] = unresolved_node_id
            nodes[unresolved_node_id]["overlays"].append(overlay)

    def overlay_at(point: list[float] | None) -> str | None:
        if point is None:
            return None
        matches = []
        for guid in grid_candidates(overlay_grid, point):
            if area_geometry_distance(source_areas[guid].get("geometry"), point, areas) <= 1e-6:
                matches.append((geometry_measure(source_areas[guid], areas), source_areas[guid]["engine_id"], guid))
        return min(matches)[2] if matches else None

    # Create floor nodes from explicit prefab counts.  Ranges use native room/door Z samples,
    # never a height-derived floor count.
    floor_nodes_by_building: dict[str, list[str]] = defaultdict(list)

    def is_descendant_of(node_id: str, ancestor_id: str) -> bool:
        seen = set()
        current = node_id
        while current in nodes and current not in seen and nodes[current].get("parent"):
            seen.add(current)
            current = nodes[current]["parent"]
            if current == ancestor_id:
                return True
        return False

    for building_id in sorted([node_id for node_id, node in nodes.items() if node["kind"] == "building"]):
        building = nodes[building_id]
        count = building["floors"]["count"]
        if not count:
            continue
        bounds = (building.get("geometry") or {}).get("bounds")
        z_samples = []
        for candidate_id, candidate in nodes.items():
            if candidate["kind"] != "room" or not is_descendant_of(candidate_id, building_id):
                continue
            candidate_bounds = (candidate.get("geometry") or {}).get("bounds")
            if candidate_bounds:
                z_samples.append((candidate_bounds["min"][2] + candidate_bounds["max"][2]) / 2)
        if building.get("_area_guids"):
            for door in doors.values():
                if door.get("position") and node_distance(building_id, door["position"]) <= 1e-6:
                    z_samples.append(float(door["position"][2]))
        ranges: list[list[float] | None] = [None] * count
        range_reason = "insufficient_native_z_samples"
        if count == 1 and bounds:
            ranges = [[bounds["min"][2], bounds["max"][2]]]
            range_reason = "single_prefab_floor"
        elif bounds and len(z_samples) >= count:
            values = sorted(z_samples)
            gaps = sorted(
                ((values[index + 1] - values[index], index) for index in range(len(values) - 1)),
                reverse=True,
            )
            cuts = sorted(index for gap, index in gaps[: count - 1] if gap >= 1.0)
            if len(cuts) == count - 1:
                boundaries = [(values[index] + values[index + 1]) / 2 for index in cuts]
                edges = [bounds["min"][2], *boundaries, bounds["max"][2]]
                ranges = [[edges[index], edges[index + 1]] for index in range(count)]
                range_reason = "native_room_and_door_z"
        for index in range(count):
            floor_id = f"{building_id}:floor:{index + 1}"
            floor = base_node(floor_id, "floor", "prefab", f"floor_{index + 1}", building_id)
            floor["parent_source"] = "prefab"
            floor["floors"] = {"count": 1, "source": "prefab", "reason": None}
            floor["z_range"] = [round_number(value) for value in ranges[index]] if ranges[index] else None
            floor["z_range_source"] = range_reason
            nodes[floor_id] = floor
            floor_nodes_by_building[building_id].append(floor_id)
        for candidate_id, candidate in list(nodes.items()):
            if candidate["kind"] != "room" or candidate["parent_source"] == "union_member":
                continue
            if not is_descendant_of(candidate_id, building_id):
                continue
            candidate_bounds = (candidate.get("geometry") or {}).get("bounds")
            if not candidate_bounds:
                continue
            z = (candidate_bounds["min"][2] + candidate_bounds["max"][2]) / 2
            for floor_id in floor_nodes_by_building[building_id]:
                z_range = nodes[floor_id]["z_range"]
                if z_range and z_range[0] - 1e-6 <= z <= z_range[1] + 1e-6:
                    candidate["parent"], candidate["parent_source"] = floor_id, "prefab"
                    break

    def floor_for_door(node_id: str, position: list[float] | None) -> str | None:
        if position is None or node_id not in floor_nodes_by_building:
            return None
        matches = [
            floor_id for floor_id in floor_nodes_by_building[node_id]
            if nodes[floor_id]["z_range"]
            and nodes[floor_id]["z_range"][0] - 1e-6 <= position[2] <= nodes[floor_id]["z_range"][1] + 1e-6
        ]
        return matches[0] if len(matches) == 1 else None

    linked_doors: dict[str, list[tuple[str, str]]] = defaultdict(list)
    linked_items: dict[str, list[tuple[str, str]]] = defaultdict(list)
    item_by_entity_id = {item["entity_id"]: guid for guid, item in items.items() if item.get("entity_id")}
    for guid, area in source_areas.items():
        for link in area["links"]:
            target_guid = link["target_guid"]
            link_kind = crime_door_kind(link["name"])
            if link_kind and target_guid in doors:
                linked_doors[target_guid].append((guid, link_kind))
            if not target_guid:
                target_guid = item_by_entity_id.get(link["target_id"], "")
            if target_guid in items:
                linked_items[target_guid].append((guid, link["name"]))

    def owner_for_area(guid: str) -> str | None:
        if guid in overlay_owner:
            return overlay_owner[guid]
        if guid in area_node:
            return structural_ancestor(area_node[guid])
        return None

    door_rows = []
    for guid, door in sorted(doors.items(), key=lambda value: value[1]["engine_id"]):
        links = linked_doors.get(guid, [])
        node_id = None
        node_source = None
        reason = None
        candidates_out: list[str] = []
        kind = None
        linked_overlay = next((area_guid for area_guid, _ in links if area_guid in overlay_info), None)
        via_overlay_guid = linked_overlay or overlay_at(door.get("position"))
        if links:
            kinds = sorted({link_kind for _, link_kind in links})
            kind = kinds[0] if len(kinds) == 1 else None
            candidates = sorted(
                {owner for area_guid, _ in links if (owner := owner_for_area(area_guid))},
                key=lambda value: (node_measure.get(value, math.inf), value),
            )
            if candidates:
                node_id, node_source = candidates[0], "crime_door_link"
                if len(kinds) > 1:
                    reason, candidates_out = "ambiguous", candidates
        if node_id is None and door.get("position") is not None:
            spatial = structural_point_candidates(door["position"], 1.5)
            if spatial:
                best = spatial[0]
                tied = [row for row in spatial if abs(row[0] - best[0]) < 1e-6 and abs(row[1] - best[1]) < 1e-6]
                if len(tied) > 1:
                    reason, candidates_out = "ambiguous", sorted({row[2] for row in tied})
                else:
                    node_id, node_source = best[2], "geometry"
        if node_id is None and via_overlay_guid in overlay_owner:
            node_id, node_source = overlay_owner[via_overlay_guid], "overlay_owner"
            if node_id == unresolved_node_id:
                reason = "overlay_without_structural_owner"
        if node_id is None:
            reason = reason or ("no_area_link" if door.get("position") is None else "outside_all_nodes")
            node_id, node_source = unresolved_node_id, "unresolved"
        if node_id:
            node_id = floor_for_door(node_id, door.get("position")) or node_id
        row = {
            "id": f"door:{door['engine_id']}",
            "engine_id": door["engine_id"],
            "name": door["name"],
            "position": round_vec3(door.get("position")),
            "kind": kind,
            "kind_source": "crime_door_link" if kind else None,
            "node": node_id,
            "node_source": node_source,
            "via_overlay": source_areas[via_overlay_guid]["engine_id"] if via_overlay_guid else None,
            "opens_to": None,
            "linked_area_engine_ids": sorted(source_areas[area_guid]["engine_id"] for area_guid, _ in links),
            "lock": door_lock_data(door),
            "reason": reason,
            "candidates": candidates_out,
        }
        door_rows.append(row)
        if node_id:
            nodes[node_id]["doors"].append(row["id"])

    entity_classes = {
        **{guid: area["entity_class"] for guid, area in areas.items()},
        **{guid: "AnimDoor" for guid in doors},
        **{guid: item["entity_class"] for guid, item in items.items()},
    }
    evidence = []
    item_rows = []
    for guid, item in sorted(items.items(), key=lambda value: value[1]["engine_id"]):
        links = linked_items.get(guid, [])
        node_id = None
        node_source = None
        linked_overlay = next((area_guid for area_guid, _ in links if area_guid in overlay_info), None)
        via_overlay_guid = linked_overlay or overlay_at(item.get("position"))
        if links:
            candidates = sorted(
                {owner for area_guid, _ in links if (owner := owner_for_area(area_guid))},
                key=lambda value: (node_measure.get(value, math.inf), value),
            )
            if candidates:
                node_id, node_source = candidates[0], "link"
                source_area, link_name = links[0]
                evidence.append({
                    "a": node_id,
                    "b": f"item:{item['engine_id']}",
                    "relation": "belongs_to_district" if "crime_stolenitemsstoragechest" in link_name.lower() else "same_building",
                    "via": "crime_link" if "crime_" in link_name.lower() else "home_equipment",
                    "source": f"{source_areas[source_area]['source_xmls'][0]}/{link_name}",
                    "used_for": "item.node",
                })
        if node_id is None and item.get("position") is not None:
            spatial = structural_point_candidates(item["position"], 2.0)
            if spatial:
                node_id, node_source = spatial[0][2], "geometry"
        item_reason = None
        if node_id is None and via_overlay_guid in overlay_owner:
            node_id, node_source = overlay_owner[via_overlay_guid], "overlay_owner"
            if node_id == unresolved_node_id:
                item_reason = "overlay_without_structural_owner"
        if node_id is None:
            node_id, node_source = unresolved_node_id, "unresolved"
            item_reason = "no_native_world_context"
        inside = bool(node_id and item.get("position") is not None and node_distance(node_id, item["position"]) <= 1.0 + 1e-6)
        item_links = [{
            "name": link["name"],
            "to_engine_id": guid_to_engine_id(link["target_guid"]) if link["target_guid"] else None,
            "to_class": entity_classes.get(link["target_guid"]),
        } for link in item["links"]]
        row = {
            "id": f"item:{item['engine_id']}",
            "engine_id": item["engine_id"],
            "class": item["entity_class"],
            "kind": item["item_kind"],
            "name": item["name"],
            "position": round_vec3(item.get("position")),
            "node": node_id,
            "node_source": node_source,
            "via_overlay": source_areas[via_overlay_guid]["engine_id"] if via_overlay_guid else None,
            "inside": inside,
            "reason": item_reason,
            "links": sorted(item_links, key=lambda value: (value["name"], value["to_engine_id"] or "")),
        }
        item_rows.append(row)
        if node_id:
            nodes[node_id]["contents"].append(row["id"])
            if nodes[node_id]["trade"] is None:
                trade = {
                    "alchemy_table": "alchemy", "blacksmithing_station": "smithing",
                    "smithery": "smithing", "grindstone": "smithing", "shop": "shop",
                }.get(row["kind"])
                if trade:
                    nodes[node_id]["trade"], nodes[node_id]["trade_source"] = trade, "feature"

    # Build inverse children, direct counts and recursive rollups.
    for node in nodes.values():
        node["doors"].sort()
        node["contents"].sort()
        node["overlays"].sort(key=lambda value: value["engine_id"])
        node["children"] = []
    for node in nodes.values():
        if node["parent"]:
            nodes[node["parent"]]["children"].append(node["id"])
    for node in nodes.values():
        node["children"].sort()

    item_by_id = {item["id"]: item for item in item_rows}
    visiting: set[str] = set()
    calculated_nodes: set[str] = set()

    def calculate_totals(node_id: str) -> dict[str, Any]:
        node = nodes[node_id]
        if node_id in visiting:
            raise RuntimeError(f"place-tree cycle at {node_id}")
        visiting.add(node_id)
        own_items = defaultdict(int)
        for item_id in node["contents"]:
            own_items[item_by_id[item_id]["kind"]] += 1
        node["own"] = {"doors": len(node["doors"]), "items_by_kind": dict(sorted(own_items.items()))}
        total_doors = len(node["doors"])
        total_items = defaultdict(int, own_items)
        total_rooms = 1 if node["kind"] == "room" else 0
        for child_id in node["children"]:
            child = calculate_totals(child_id)
            total_doors += child["doors"]
            total_rooms += child["rooms"]
            for kind, value in child["items_by_kind"].items():
                total_items[kind] += value
        node["totals"] = {
            "doors": total_doors,
            "items_by_kind": dict(sorted(total_items.items())),
            "rooms": total_rooms,
        }
        visiting.remove(node_id)
        calculated_nodes.add(node_id)
        return node["totals"]

    calculate_totals(level_node_id)
    if calculated_nodes != set(nodes):
        missing = sorted(set(nodes) - calculated_nodes)
        chain = []
        current = missing[0]
        while current in nodes and current not in chain and len(chain) < 100:
            chain.append(current)
            current = nodes[current].get("parent")
        chain.append(current)
        raise RuntimeError(
            f"place-tree nodes are not reachable from {level_node_id}: {missing[:10]!r}; "
            f"first parent chain={chain!r}"
        )
    for node in nodes.values():
        node.pop("_area_guids", None)
        node.pop("_site_path", None)

    node_rows = sorted(nodes.values(), key=lambda value: value["id"])
    door_rows.sort(key=lambda value: value["id"])
    item_rows.sort(key=lambda value: value["id"])
    evidence.sort(key=lambda value: (value["a"], value["b"], value["relation"], value["via"]))
    kinds = ("level", "settlement", "district", "compound", "castle", "courtyard", "building", "floor", "room", "outdoor", "other")
    sources = ("site_tree", "union_member", "geometry", "location", "label", "prefab", "unresolved")
    overlay_patterns = [
        {"kind": kind, "pattern": pattern, "count": count}
        for (kind, pattern), count in sorted(overlay_pattern_counts.items())
    ]
    exterior_items = sum(
        item["node"] is not None and nodes[item["node"]]["kind"] == "settlement"
        for item in item_rows
    )
    exterior_doors = sum(
        door["node"] is not None and nodes[door["node"]]["kind"] == "settlement"
        for door in door_rows
    )
    counts = {
        "nodes": len(node_rows),
        "nodes_by_kind": {kind: sum(node["kind"] == kind for node in node_rows) for kind in kinds},
        "nodes_by_parent_source": {source: sum(node["parent_source"] == source for node in node_rows) for source in sources},
        "nodes_without_parent": sum(node["kind"] != "level" and node["parent"] is None for node in node_rows),
        "doors": len(door_rows),
        "doors_with_node": sum(door["node"] is not None for door in door_rows),
        "doors_without_node": sum(door["node"] is None for door in door_rows),
        "doors_without_node_by_reason": {
            reason: sum(door["node"] is None and door["reason"] == reason for door in door_rows)
            for reason in ("no_area_link", "outside_all_nodes", "ambiguous")
        },
        "items": len(item_rows),
        "items_by_kind": {
            kind: sum(item["kind"] == kind for item in item_rows)
            for kind in sorted({item["kind"] for item in item_rows})
        },
        "items_with_node": sum(item["node"] is not None for item in item_rows),
        "items_on_structural_nodes": sum(item["node"] is not None and structural_node_kind(nodes[item["node"]]["kind"]) for item in item_rows),
        "overlays": len(overlay_info),
        "overlays_by_kind": dict(sorted((kind, sum(value[0] == kind for value in overlay_info.values())) for kind in {value[0] for value in overlay_info.values()})),
        "overlay_patterns": overlay_patterns,
        "overlays_without_owner": len(unresolved_overlays),
        "castle_site_merges": castle_merges,
        "items_on_settlement_exterior": exterior_items,
        "doors_on_settlement_exterior": exterior_doors,
        "unresolved_doors": sum(door["reason"] is not None for door in door_rows),
        "unresolved_items": sum(item["reason"] is not None for item in item_rows),
        "areas_excluded_by_class": dict(sorted(excluded_counts.items())),
        "areas_without_node": len(place_areas) - len(area_node),
        "excluded_site_segments": dict(sorted(excluded_segments.items())),
    }
    unresolved_doors = [door for door in door_rows if door["reason"] is not None]
    unresolved_items = [item for item in item_rows if item["reason"] is not None]
    payload = {
        "schema": TREE_SCHEMA,
        "source": {"game_build": build_info},
        "level": {"id": level_id, "directory": level_name},
        "counts": counts,
        "limitations": {
            "doors_without_node_examples": [door["id"] for door in unresolved_doors[:10]],
            "items_without_node_examples": [item["id"] for item in unresolved_items[:10]],
            "overlays_without_owner_examples": unresolved_overlays[:10],
            "unresolved_doors": [
                {"id": door["id"], "reason": door["reason"]} for door in unresolved_doors
            ],
            "unresolved_items": [
                {"id": item["id"], "reason": item["reason"]} for item in unresolved_items
            ],
            "overlays_without_owner": unresolved_overlays,
            "missing_item_classes": [],
        },
        "nodes": node_rows,
        "doors": door_rows,
        "items": item_rows,
        "evidence": evidence,
    }
    return payload, counts


def export_level(
    game_root: Path,
    level_name: str,
    level_id: str,
    output: Path,
    property_output: Path,
    tree_output: Path,
    strings: dict[str, dict[str, str]],
    location_types: dict[str, dict[str, str]],
    poi_types: dict[str, dict[str, str]],
    build_info: dict[str, Any],
) -> tuple[dict[str, int], dict[str, Any], dict[str, Any]]:
    level_pak = game_root / "Data" / "Levels" / level_name / "level.pak"
    if not level_pak.is_file():
        raise FileNotFoundError(level_pak)
    areas: dict[str, dict[str, Any]] = {}
    features: dict[str, dict[str, Any]] = {}
    doors: dict[str, dict[str, Any]] = {}
    items: dict[str, dict[str, Any]] = {}
    pois: list[dict[str, Any]] = []
    site_evidence: dict[str, dict[str, set[str]]] = defaultdict(lambda: {"tags": set(), "entity_guids": set()})
    with zipfile.ZipFile(level_pak) as archive:
        fubar_header, trigger_records = parse_triggerareas(read_zip_entry(archive, "triggerareas.fubar"))
        names = ["objects_mission0.xml"] + sorted(
            name for name in archive.namelist()
            if name.lower().startswith("layers/") and name.lower().endswith(".xml") and archive.getinfo(name).file_size > 12
        )
        parsed_documents = 0
        for name in names:
            try:
                parse_document(read_zip_entry(archive, name), name, areas, features, doors, items, pois, site_evidence)
                parsed_documents += 1
            except ET.ParseError as error:
                raise ValueError(f"could not parse {level_name}/{name}: {error}") from error

    for trigger in trigger_records:
        current = areas.get(trigger["guid"])
        reference_z = (
            current["position"][2]
            if current and current.get("position") is not None
            else trigger["polygon_world"][0][2]
        )
        geometry = {
            "kind": "polygon_prism",
            "coordinate_space": "world",
            "height": trigger["height"],
            "polygon_world": trigger["polygon_world"],
            "area_2d": polygon_area(trigger["polygon_world"]),
            "bounding_box": geometry_bbox(trigger["polygon_world"], trigger["height"], reference_z),
            "source": "triggerareas.fubar",
        }
        if current:
            current["geometry"] = geometry
        else:
            areas[trigger["guid"]] = {
                "guid": trigger["guid"], "guid_u64": trigger["guid_u64"], "entity_id": "",
                "engine_id": f"{int(trigger['guid_u64']):016x}",
                "name": "", "entity_class": "TriggerArea", "editor_layer": "", "site_path": "",
                "position": trigger["polygon_world"][0], "rotation": [1.0, 0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0], "labels": [], "properties": {}, "links": [],
                "geometry": geometry, "source_xmls": [],
            }

    by_id = {
        row["entity_id"]: row["guid"]
        for collection in (areas.values(), features.values(), doors.values())
        for row in collection
        if row.get("entity_id")
    }
    all_by_id = {
        **by_id,
        **{item["entity_id"]: item["guid"] for item in items.values() if item.get("entity_id")},
    }
    for area in areas.values():
        for link in area["links"]:
            if not link["target_guid"] and link["target_id"] in by_id:
                link["target_guid"] = by_id[link["target_id"]]
                link["target_engine_id"] = guid_to_engine_id(link["target_guid"])
    for item in items.values():
        for link in item["links"]:
            if not link["target_guid"] and link["target_id"] in all_by_id:
                link["target_guid"] = all_by_id[link["target_id"]]
                link["target_engine_id"] = guid_to_engine_id(link["target_guid"])
    member_of: dict[str, set[str]] = defaultdict(set)
    for area in areas.values():
        members = []
        for link in area["links"]:
            if link["name"].lower().startswith("area_union_member"):
                target = link["target_guid"] or by_id.get(link["target_id"], "")
                if target:
                    members.append(target)
                    member_of[target].add(area["guid"])
        area["relations"] = {
            "union_member_guids": sorted(set(members)),
            "member_of_union_guids": [],
        }
    for guid, parents in member_of.items():
        if guid in areas:
            areas[guid]["relations"]["member_of_union_guids"] = sorted(parents)
    for area in areas.values():
        if area["entity_class"] == "AreaUnion":
            aggregate_union_geometry(area, areas)

    location_area_guids = []
    for area in areas.values():
        properties_blob = " ".join(str(value) for value in area["properties"].values())
        own_tags = infer_tags(area["site_path"], area["editor_layer"], area["name"], area["entity_class"], properties_blob, " ".join(area["labels"]))
        if area["properties"].get("guidLocationId"):
            own_tags.update(("location", "poi"))
            location_area_guids.append(area["guid"])
        # Propagate evidence only within the same native site.  Promoting a
        # blacksmith nested somewhere under ``main`` to every descendant of
        # ``main`` would incorrectly classify almost the whole level as a job.
        inherited = set(site_evidence.get(area["site_path"], {}).get("tags", set()))
        tags = own_tags | inherited
        area["derived"] = {
            "semantic_tags": sorted(tags),
            "semantic_tags_legacy": sorted(tags),
            "requested_scope": bool(tags & REQUESTED_TAGS),
            "site_ancestors": site_ancestors(area["site_path"]),
        }
        area["labels"] = labels_from_properties(area["properties"])

    location_areas = [areas[guid] for guid in location_area_guids]
    for area in areas.values():
        position = area["position"]
        if not position and area.get("geometry", {}).get("bounding_box"):
            box = area["geometry"]["bounding_box"]
            position = [(box["min"][axis] + box["max"][axis]) / 2 for axis in range(3)]
        area["relations"]["containing_location_area_guids"] = [
            guid for guid in containment(location_areas, position) if guid != area["guid"]
        ]

    map_marker_types = []
    marker_tags_by_id: dict[str, set[str]] = {}
    for poi_type_id, row in sorted(poi_types.items()):
        tags = infer_tags(row.get("poi_type_name", ""), row.get("label", ""))
        marker_tags_by_id[poi_type_id] = tags
        map_marker_types.append({
            **row,
            "display_label": resolve_label(row.get("label", ""), strings),
            "semantic_tags": sorted(tags),
            "requested_scope": bool(tags & REQUESTED_TAGS),
        })

    area_values = list(areas.values())
    feature_tags_by_area: dict[str, set[str]] = defaultdict(set)
    for feature in features.values():
        all_containing_area_guids = containment(area_values, feature["position"], limit=None)
        feature["containing_area_guids"] = all_containing_area_guids[:32]
        feature_tags = set(feature["semantic_tags"])
        if feature["position"] is not None:
            for guid in all_containing_area_guids:
                area = areas[guid]
                if area_geometry_distance(area.get("geometry"), feature["position"], areas) <= 1e-6:
                    feature_tags_by_area[area["guid"]].update(feature_tags)
        feature["matching_map_marker_type_ids"] = [
            poi_type_id for poi_type_id, tags in marker_tags_by_id.items()
            if tags and tags & feature_tags
        ]
        feature.pop("geometry", None)
        feature.pop("links", None)
        feature.pop("properties", None)

    for area in area_values:
        tags = tags_from_site_path(area["site_path"]) | feature_tags_by_area[area["guid"]]
        if area["properties"].get("guidLocationId"):
            tags.update(("location", "poi"))
        area["derived"]["semantic_tags"] = sorted(tags)
        area["derived"]["requested_scope"] = bool(tags & REQUESTED_TAGS)
    for poi in pois:
        poi_type = poi_types.get(poi["poi_type_id"], {})
        poi["poi_type"] = poi_type
        label_key = poi["custom_label_key"] or poi_type.get("label", "")
        poi["display_label"] = resolve_label(label_key, strings)
        poi["semantic_tags"] = sorted(infer_tags(poi["name"], poi_type.get("poi_type_name", ""), poi_type.get("label", "")) | {"poi"})
        poi["requested_scope"] = bool(set(poi["semantic_tags"]) & REQUESTED_TAGS)
        poi["containing_area_guids"] = containment(area_values, poi["position"])

    locations = []
    for guid in location_area_guids:
        area = areas[guid]
        location_id = str(area["properties"].get("guidLocationId", "")).lower()
        row = location_types.get(location_id, {})
        locations.append({
            "location_id": location_id,
            "area_guid": guid,
            "engine_id": area["engine_id"],
            "member_area_ids": sorted(
                candidate["engine_id"]
                for candidate in areas.values()
                if guid in candidate["relations"]["containing_location_area_guids"]
            ),
            "name_key": row.get("location_name", ""),
            "display_label": resolve_label(row.get("location_name", ""), strings),
            "icon_id": row.get("icon_id", ""),
            "ui_type": row.get("ui_type", ""),
            "native_table_row": row,
        })

    site_nodes: dict[str, dict[str, Any]] = {}
    for area in areas.values():
        for path in site_ancestors(area["site_path"]):
            site_nodes.setdefault(path, {"direct_area_guids": [], "direct_feature_guids": [], "semantic_tags": set()})
        if area["site_path"]:
            site_nodes[area["site_path"]]["direct_area_guids"].append(area["guid"])
            site_nodes[area["site_path"]]["semantic_tags"].update(area["derived"]["semantic_tags"])
    for feature in features.values():
        for path in site_ancestors(feature["site_path"]):
            site_nodes.setdefault(path, {"direct_area_guids": [], "direct_feature_guids": [], "semantic_tags": set()})
        if feature["site_path"]:
            site_nodes[feature["site_path"]]["direct_feature_guids"].append(feature["guid"])
            site_nodes[feature["site_path"]]["semantic_tags"].update(feature["semantic_tags"])
    sites = []
    for path, node in sorted(site_nodes.items()):
        parent = path.rpartition("/")[0]
        tags = set(node["semantic_tags"]) | site_evidence.get(path, {}).get("tags", set())
        sites.append({
            "site_id": f"{level_id}:{path}",
            "path": path,
            "name": path.rpartition("/")[2],
            "parent_site_id": f"{level_id}:{parent}" if parent else "",
            "semantic_tags": sorted(tags),
            "requested_scope": bool(tags & REQUESTED_TAGS),
            "direct_area_guids": sorted(set(node["direct_area_guids"])),
            "direct_feature_guids": sorted(set(node["direct_feature_guids"])),
        })

    property_payload, property_counts = build_property_catalog(
        level_name, level_id, build_info, areas, doors, features, locations
    )
    tree_payload, tree_counts = build_place_tree(
        level_name, level_id, build_info, areas, doors, items, locations
    )

    area_list = sorted(areas.values(), key=lambda value: value["engine_id"])
    feature_list = sorted(features.values(), key=lambda value: value["engine_id"])
    poi_list = sorted(pois, key=lambda value: value["poi_id"])
    locations.sort(key=lambda value: value["location_id"])
    payload = {
        "schema": SCHEMA,
        "generated_utc": stable_generated_utc(build_info),
        "level": {"id": level_id, "directory": level_name},
        "source": {
            "game_build": build_info,
            "level_pak": str(level_pak),
            "level_pak_size": level_pak.stat().st_size,
            "parsed_xml_documents": parsed_documents,
            "triggerareas": fubar_header,
        },
        "scope": {
            "note": "All native areas are retained. Use derived.requested_scope or semantic_tags for the requested jobs/buildings/rooms/settlements subset.",
            "requested_tags": sorted(REQUESTED_TAGS),
            "native_vs_derived": "GUIDs, entity fields, labels, links and geometry are native; site hierarchy, semantic tags and spatial containment are derived.",
        },
        "counts": {
            "areas": len(area_list),
            "requested_areas": sum(area["derived"]["requested_scope"] for area in area_list),
            "locations": len(locations),
            "points_of_interest": len(poi_list),
            "requested_points_of_interest": sum(poi["requested_scope"] for poi in poi_list),
            "map_marker_types": len(map_marker_types),
            "requested_map_marker_types": sum(marker["requested_scope"] for marker in map_marker_types),
            "workstations_and_shops": len(feature_list),
            "sites": len(sites),
            "areas_without_geometry": sum(not area.get("geometry") for area in area_list),
        },
        "locations": locations,
        "map_marker_types": map_marker_types,
        "points_of_interest": poi_list,
        "workstations_and_shops": feature_list,
        "sites": sites,
        "areas": area_list,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    # Parse the finished artifact once so truncation or encoding failures do not
    # silently produce a half-valid dump.
    with output.open("r", encoding="utf-8") as stream:
        verified = json.load(stream)
    if verified["counts"] != payload["counts"]:
        raise RuntimeError(f"JSON verification failed for {output}")
    property_output.write_text(
        json.dumps(property_payload, ensure_ascii=False, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    with property_output.open("r", encoding="utf-8") as stream:
        verified_properties = json.load(stream)
    if verified_properties["counts"] != property_counts:
        raise RuntimeError(f"JSON verification failed for {property_output}")
    tree_output.write_text(
        json.dumps(tree_payload, ensure_ascii=False, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    with tree_output.open("r", encoding="utf-8") as stream:
        verified_tree = json.load(stream)
    if verified_tree["counts"] != tree_counts:
        raise RuntimeError(f"JSON verification failed for {tree_output}")
    return payload["counts"], property_counts, tree_counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game_root", type=Path)
    parser.add_argument("--output-directory", type=Path, default=Path("out/kcd2-area-catalog"))
    parser.add_argument(
        "--levels",
        nargs="*",
        choices=("trosecko", "kutnohorsko", "klaster"),
        default=("trosecko", "kutnohorsko", "klaster"),
    )
    args = parser.parse_args()
    strings = localized_strings(args.game_root)
    build_info = game_build_info(args.game_root)
    with zipfile.ZipFile(args.game_root / "Data" / "Tables.pak") as tables:
        locations = table_rows(tables, "Libs/Tables/rpg/location.xml", "location", "location_id")
        poi_types = table_rows(tables, "Libs/Tables/rpg/poi_type.xml", "poi_type", "poi_type_id")
    level_ids = {"trosecko": "2", "kutnohorsko": "3", "klaster": "4"}
    manifest = {"schema": SCHEMA, "generated_utc": stable_generated_utc(build_info), "game_build": build_info, "levels": []}
    for level in args.levels:
        output = args.output_directory / f"areas_{level}.json"
        property_output = args.output_directory / f"properties_{level}.json"
        tree_output = args.output_directory / f"tree_{level}.json"
        counts, property_counts, tree_counts = export_level(
            args.game_root, level, level_ids[level], output, property_output, tree_output,
            strings, locations, poi_types, build_info,
        )
        manifest["levels"].append({
            "id": level_ids[level],
            "directory": level,
            "file": output.name,
            "counts": counts,
            "properties_file": property_output.name,
            "property_counts": property_counts,
            "tree_file": tree_output.name,
            "tree_schema": TREE_SCHEMA,
            "tree_counts": tree_counts,
            "tree_overlay_patterns": tree_counts["overlay_patterns"],
            "tree_castle_site_merges": tree_counts["castle_site_merges"],
        })
        print(f"Exported {level}: {counts['areas']} areas, {counts['requested_areas']} requested-scope areas -> {output}")
        print(f"Exported {level}: {property_counts['properties']} properties -> {property_output}")
        print(f"Exported {level}: {tree_counts['nodes']} place nodes, {tree_counts['doors']} doors, {tree_counts['items']} items -> {tree_output}")
    args.output_directory.mkdir(parents=True, exist_ok=True)
    manifest_path = args.output_directory / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote manifest: {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
