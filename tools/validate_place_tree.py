#!/usr/bin/env python3
"""Validate kcd2-place-tree/v2 files and print the structural acceptance report."""

from __future__ import annotations

import argparse
import json
import math
from collections import Counter
from pathlib import Path
from typing import Any

try:
    from tools.export_area_catalog import (
        STRUCTURAL_KINDS,
        area_geometry_distance,
        area_is_excluded_from_tree,
        build_spatial_grid,
        grid_candidates,
        tree_overlay_kind,
    )
except ModuleNotFoundError:  # Direct ``python tools/validate_place_tree.py`` execution.
    from export_area_catalog import (
        STRUCTURAL_KINDS,
        area_geometry_distance,
        area_is_excluded_from_tree,
        build_spatial_grid,
        grid_candidates,
        tree_overlay_kind,
    )


def load(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def ancestors(node_id: str, nodes: dict[str, dict[str, Any]]) -> list[str]:
    result = []
    seen = set()
    current = node_id
    while current in nodes and nodes[current].get("parent"):
        current = nodes[current]["parent"]
        if current in seen:
            result.append(current)
            break
        seen.add(current)
        result.append(current)
    return result


def display_name(node: dict[str, Any]) -> str:
    name = node["name"]
    return name.get("de") or name.get("en") or name.get("raw") or node["id"]


def expected_tree_counts(tree: dict[str, Any]) -> dict[str, Any]:
    nodes, doors, items = tree["nodes"], tree["doors"], tree["items"]
    kinds = (
        "level", "settlement", "district", "compound", "castle", "courtyard",
        "building", "floor", "room", "outdoor", "other",
    )
    sources = ("site_tree", "union_member", "location", "geometry", "label", "prefab", "unresolved")
    return {
        "nodes": len(nodes),
        "nodes_by_kind": {kind: sum(node["kind"] == kind for node in nodes) for kind in kinds},
        "nodes_by_parent_source": {
            source: sum(node["parent_source"] == source for node in nodes) for source in sources
        },
        "nodes_without_parent": sum(node["kind"] != "level" and node["parent"] is None for node in nodes),
        "doors": len(doors),
        "doors_with_node": sum(door["node"] is not None for door in doors),
        "doors_without_node": sum(door["node"] is None for door in doors),
        "doors_without_node_by_reason": {
            reason: sum(door["node"] is None and door["reason"] == reason for door in doors)
            for reason in ("no_area_link", "outside_all_nodes", "ambiguous")
        },
        "items": len(items),
        "items_by_kind": {
            kind: sum(item["kind"] == kind for item in items)
            for kind in sorted({item["kind"] for item in items})
        },
        "items_with_node": sum(item["node"] is not None for item in items),
        "areas_excluded_by_class": tree["counts"]["areas_excluded_by_class"],
        "areas_without_node": tree["counts"]["areas_without_node"],
        "excluded_site_segments": tree["counts"]["excluded_site_segments"],
    }


def print_subtree(
    node_id: str,
    nodes: dict[str, dict[str, Any]],
    depth: int = 0,
    maximum_depth: int = 5,
) -> None:
    node = nodes[node_id]
    visible_kinds = {"settlement", "castle", "courtyard", "building", "floor", "room"}
    visible = node["kind"] in visible_kinds
    flags = []
    if node["parent_source"] is None and node["kind"] != "level":
        flags.append("NULL parent_source")
    suffix = f" [{' | '.join(flags)}]" if flags else ""
    next_depth = depth
    if visible:
        print(
            f"{'  ' * depth}- {display_name(node)} "
            f"({node['kind']}, parent_source={node['parent_source']}, "
            f"own={node['own']} totals={node['totals']}){suffix}"
        )
        next_depth += 1
    if next_depth >= maximum_depth:
        return
    for child in node["children"]:
        print_subtree(child, nodes, next_depth, maximum_depth)


def validate(catalog_directory: Path, fixture_path: Path) -> int:
    fixture = load(fixture_path)
    manifest = load(catalog_directory / "manifest.json")
    errors: list[str] = []
    trees: dict[str, dict[str, Any]] = {}

    for manifest_level in manifest["levels"]:
        level = manifest_level["directory"]
        tree_path = catalog_directory / f"tree_{level}.json"
        area_path = catalog_directory / f"areas_{level}.json"
        if not tree_path.is_file():
            errors.append(f"missing {tree_path.name}")
            continue
        tree, area_catalog = load(tree_path), load(area_path)
        trees[level] = tree
        if tree.get("schema") != "kcd2-place-tree/v2":
            errors.append(f"{level}: wrong schema {tree.get('schema')!r}")
        nodes = {node["id"]: node for node in tree["nodes"]}
        if len(nodes) != len(tree["nodes"]):
            errors.append(f"{level}: duplicate node IDs")
        membership = Counter(area_id for node in tree["nodes"] for area_id in node["area_ids"])
        overlay_membership = Counter(
            overlay["engine_id"] for node in tree["nodes"] for overlay in node.get("overlays", [])
        )
        expected_areas = [
            area for area in area_catalog["areas"]
            if not area_is_excluded_from_tree(area) and tree_overlay_kind(area) is None
        ]
        expected_overlays = [
            area for area in area_catalog["areas"]
            if not area_is_excluded_from_tree(area) and tree_overlay_kind(area) is not None
        ]
        for area in expected_areas:
            if membership[area["engine_id"]] != 1:
                errors.append(
                    f"{level}: area {area['engine_id']} belongs to {membership[area['engine_id']]} nodes"
                )
        for area in expected_overlays:
            if membership[area["engine_id"]] != 0 or overlay_membership[area["engine_id"]] != 1:
                errors.append(
                    f"{level}: overlay {area['engine_id']} node_count={membership[area['engine_id']]} "
                    f"overlay_count={overlay_membership[area['engine_id']]}"
                )
        unexpected = set(membership) - {area["engine_id"] for area in expected_areas}
        if unexpected:
            errors.append(f"{level}: excluded/unknown areas occur in nodes: {sorted(unexpected)[:10]!r}")

        expected_children: dict[str, list[str]] = {node_id: [] for node_id in nodes}
        level_nodes = [node for node in tree["nodes"] if node["kind"] == "level"]
        if len(level_nodes) != 1 or level_nodes[0]["parent"] is not None:
            errors.append(f"{level}: expected exactly one root level node")
        for node in tree["nodes"]:
            if node["kind"] != "level":
                if node["parent"] not in nodes:
                    errors.append(f"{level}: node {node['id']} has invalid parent {node['parent']!r}")
                else:
                    expected_children[node["parent"]].append(node["id"])
            chain = ancestors(node["id"], nodes)
            if len(chain) != len(set(chain)):
                errors.append(f"{level}: cycle at node {node['id']}")
        for node_id, node in nodes.items():
            if sorted(node["children"]) != sorted(expected_children[node_id]):
                errors.append(f"{level}: children are not inverse at {node_id}")
            own_items = Counter(
                item["kind"] for item in tree["items"] if item["id"] in set(node["contents"])
            )
            expected_own = {"doors": len(node["doors"]), "items_by_kind": dict(sorted(own_items.items()))}
            if node.get("own") != expected_own:
                errors.append(f"{level}: own rollup mismatch at {node_id}")

        item_by_id = {item["id"]: item for item in tree["items"]}
        memo: dict[str, dict[str, Any]] = {}
        ordered_node_ids = sorted(nodes, key=lambda value: len(ancestors(value, nodes)), reverse=True)
        for node_id in ordered_node_ids:
            node = nodes[node_id]
            kinds = Counter(item_by_id[item_id]["kind"] for item_id in node["contents"])
            door_count = len(node["doors"])
            room_count = int(node["kind"] == "room")
            for child_id in node["children"]:
                child = memo.get(child_id)
                if child is None:
                    errors.append(f"{level}: cannot calculate totals below cycle/depth at {node_id}")
                    continue
                door_count += child["doors"]
                room_count += child["rooms"]
                kinds.update(child["items_by_kind"])
            memo[node_id] = {
                "doors": door_count,
                "items_by_kind": dict(sorted(kinds.items())),
                "rooms": room_count,
            }
        for node_id, node in nodes.items():
            if node.get("totals") != memo.get(node_id):
                errors.append(f"{level}: totals rollup mismatch at {node_id}")

        invalid_items = [
            item["id"] for item in tree["items"]
            if item["node"] not in nodes or nodes[item["node"]]["kind"] not in STRUCTURAL_KINDS
        ]
        invalid_doors = [
            door["id"] for door in tree["doors"]
            if door["node"] not in nodes
            or nodes[door["node"]]["kind"] not in STRUCTURAL_KINDS | {"floor"}
        ]
        if invalid_items:
            errors.append(f"{level}: non-structural item owners: {invalid_items[:10]!r}")
        if invalid_doors:
            errors.append(f"{level}: non-structural door owners: {invalid_doors[:10]!r}")
        for node in tree["nodes"]:
            if node.get("overlays") and node["kind"] not in STRUCTURAL_KINDS:
                errors.append(f"{level}: overlays attached to non-structural node {node['id']}")
            floor_count = node.get("floors", {}).get("count")
            if node["kind"] == "building" and floor_count:
                floor_children = [child for child in node["children"] if nodes[child]["kind"] == "floor"]
                if len(floor_children) != floor_count:
                    errors.append(
                        f"{level}: building {node['id']} has {len(floor_children)}/{floor_count} floor nodes"
                    )
        for floor in (node for node in tree["nodes"] if node["kind"] == "floor"):
            if floor.get("z_range") is None and not floor.get("z_range_source"):
                errors.append(f"{level}: floor {floor['id']} has null z_range without reason")
        if manifest_level.get("tree_file") != tree_path.name:
            errors.append(f"{level}: manifest tree_file mismatch")
        if manifest_level.get("tree_schema") != "kcd2-place-tree/v2":
            errors.append(f"{level}: manifest tree_schema mismatch")
        if manifest_level.get("tree_counts") != tree["counts"]:
            errors.append(f"{level}: manifest tree_counts mismatch")
        if manifest_level.get("tree_overlay_patterns") != tree["counts"]["overlay_patterns"]:
            errors.append(f"{level}: manifest overlay pattern mismatch")
        if manifest_level.get("tree_castle_site_merges") != tree["counts"]["castle_site_merges"]:
            errors.append(f"{level}: manifest castle merge mismatch")
        print(
            f"{level}: {len(nodes)} nodes, {len(tree['doors'])} doors, "
            f"{len(tree['items'])} items, {len(expected_overlays)} overlays; membership exact"
        )

    level = str(fixture.get("level") or "kutnohorsko").lower()
    tree = trees.get(level)
    if tree:
        nodes = {node["id"]: node for node in tree["nodes"]}
        node_by_area = {
            area_id: node["id"]
            for node in tree["nodes"]
            for area_id in node["area_ids"]
        }
        node_by_overlay = {
            overlay["engine_id"]: node["id"]
            for node in tree["nodes"]
            for overlay in node.get("overlays", [])
        }
        area_catalog = load(catalog_directory / f"areas_{level}.json")
        catalog_area = {area["engine_id"]: area for area in area_catalog["areas"]}
        fixture_included = [
            area for area in fixture["areas"]
            if area["id"] in catalog_area and not area_is_excluded_from_tree(catalog_area[area["id"]])
        ]
        matched = sum(area["id"] in node_by_area or area["id"] in node_by_overlay for area in fixture_included)
        if matched != len(fixture_included):
            errors.append(f"fixture areas matched {matched}/{len(fixture_included)}")
        print(f"Fixture areas: {matched}/{len(fixture_included)} assigned exactly once")

        tree_doors = {door["engine_id"]: door for door in tree["doors"]}
        matched_doors = sum(door["guid"] in tree_doors for door in fixture["doors"])
        if matched_doors != len(fixture["doors"]):
            errors.append(f"fixture doors matched {matched_doors}/{len(fixture['doors'])}")
        linked_checks = 0
        for area in fixture["areas"]:
            if not area.get("linked") or (area["id"] not in node_by_area and area["id"] not in node_by_overlay):
                continue
            area_node_id = node_by_area.get(area["id"]) or node_by_overlay[area["id"]]
            for expected in area["linked"]:
                actual = tree_doors.get(expected["guid"])
                if actual is None:
                    continue
                if actual["node_source"] != "crime_door_link":
                    errors.append(f"door {expected['guid']} is not assigned by crime_door_link")
                if actual["kind"] != expected["link"]:
                    errors.append(
                        f"door {expected['guid']} kind {actual['kind']!r} != {expected['link']!r}"
                    )
                linked_nodes = {
                    node_by_area.get(eid) or node_by_overlay[eid]
                    for eid in actual["linked_area_engine_ids"]
                    if eid in node_by_area or eid in node_by_overlay
                }
                actual_lineage = [actual["node"], *ancestors(actual["node"], nodes)]
                if not linked_nodes.intersection(actual_lineage) and area_node_id not in actual_lineage:
                    errors.append(f"door {expected['guid']} is not on a linked area/house node")
                linked_checks += 1
        print(f"Fixture doors: {matched_doors}/{len(fixture['doors'])}; linked checks: {linked_checks}")

        union_checks = 0
        for union in fixture["areas"]:
            if not union.get("members") or union["id"] not in node_by_area:
                continue
            union_node = node_by_area[union["id"]]
            for member_id in union["members"]:
                if member_id not in node_by_area:
                    errors.append(f"union member {member_id} has no node")
                elif nodes[node_by_area[member_id]]["parent"] != union_node:
                    errors.append(f"union member {member_id} is not a direct child of {union['id']}")
                union_checks += 1
        print(f"Union member checks: {union_checks}")

        # The measured regression was buildings left directly at a Location even
        # though a smaller enclosing castle/compound/courtyard existed.
        area_by_guid = {area["guid"]: area for area in area_catalog["areas"]}
        spatial_grid = build_spatial_grid(
            (area["engine_id"], area.get("geometry")) for area in area_catalog["areas"]
        )

        def node_measure(node: dict[str, Any]) -> float:
            values = []
            for engine_id in node["area_ids"]:
                geometry = (catalog_area[engine_id].get("geometry") or {})
                bbox = geometry.get("bounding_box")
                if geometry.get("area_2d") is not None:
                    values.append(float(geometry["area_2d"]))
                elif bbox:
                    values.append(math.prod(
                        max(0.001, bbox["max"][axis] - bbox["min"][axis]) for axis in range(3)
                    ))
            return min(values) if values else math.inf

        building_violations = []
        for building in tree["nodes"]:
            if building["kind"] != "building" or building["parent_source"] != "location":
                continue
            bounds = (building.get("geometry") or {}).get("bounds")
            parent = nodes.get(building["parent"])
            if not bounds or not parent:
                continue
            point = [(bounds["min"][axis] + bounds["max"][axis]) / 2 for axis in range(3)]
            own_measure, parent_measure = node_measure(building), node_measure(parent)
            checked_nodes = set()
            for engine_id in grid_candidates(spatial_grid, point):
                candidate_id = node_by_area.get(engine_id)
                if not candidate_id or candidate_id in checked_nodes:
                    continue
                checked_nodes.add(candidate_id)
                candidate = nodes[candidate_id]
                candidate_measure = node_measure(candidate)
                if (
                    candidate["kind"] not in STRUCTURAL_KINDS
                    or candidate_id == building["id"]
                    or not own_measure < candidate_measure < parent_measure
                ):
                    continue
                if any(
                    area_geometry_distance(catalog_area[value].get("geometry"), point, area_by_guid) <= 1e-6
                    for value in candidate["area_ids"]
                ):
                    building_violations.append((building["id"], candidate_id))
                    break
        if building_violations:
            errors.append(f"building containment violations: {building_violations[:10]!r}")
        print(f"Building containment violations: {len(building_violations)}")

        linked_areas = sum(bool(area.get("linked")) for area in fixture["areas"])
        roof_overlay_ids = {
            overlay["engine_id"]
            for node in tree["nodes"]
            for overlay in node.get("overlays", [])
            if overlay["kind"] == "roof"
        }
        roof_link_doors = sum(
            bool(set(door["linked_area_engine_ids"]) & roof_overlay_ids)
            for door in tree["doors"]
        )
        print(f"Linked fixture areas: {linked_areas}; roof-linked doors resolved below house: {roof_link_doors}")

    placed_items_with_context = []
    for tree_level, tree in trees.items():
        nodes = {node["id"]: node for node in tree["nodes"]}
        for item in tree["items"]:
            if not item["node"]:
                continue
            lineage = [item["node"], *ancestors(item["node"], nodes)]
            settlement = next(
                (display_name(nodes[node_id]) for node_id in lineage if nodes[node_id]["kind"] == "settlement"),
                None,
            )
            placed_items_with_context.append((tree_level, settlement, item))
    placed_items = [item for _, _, item in placed_items_with_context]
    if len(placed_items) < 20:
        errors.append(f"only {len(placed_items)} positioned items have a node")
    sample_kinds = ("smithery", "alchemy_table", "chest")
    target_places = ("suchdol", "sukdol", "kutten", "kutna", "sedlec", "sedletz")
    prioritized = sorted(
        (row for row in placed_items_with_context if row[2]["kind"] in sample_kinds),
        key=lambda row: (
            0 if any(word in (row[1] or "").lower() for word in target_places) else 1,
            sample_kinds.index(row[2]["kind"]),
            row[0],
            row[2]["id"],
        ),
    )
    samples = []
    seen_contexts = set()
    for row in prioritized:
        context = (row[1], row[2]["kind"])
        if context not in seen_contexts:
            samples.append(row)
            seen_contexts.add(context)
        if len(samples) == 20:
            break
    for row in prioritized:
        if len(samples) == 20:
            break
        if row not in samples:
            samples.append(row)
    if not all(any(item[2]["kind"] == kind for item in samples) for kind in sample_kinds):
        errors.append("20-item sample does not cover smithery, alchemy_table, and chest")
    print("Item sample (20 smithery/alchemy/chest entities, prioritizing Sukdol/Kuttenberg/Sedlec):")
    for tree_level, settlement, item in samples:
        print(
            f" - {item['id']} level={tree_level} settlement={settlement!r} "
            f"kind={item['kind']} node={item['node']} "
            f"source={item['node_source']} inside={item['inside']}"
        )

    kut_tree = trees.get("kutnohorsko")
    if kut_tree:
        nodes = {node["id"]: node for node in kut_tree["nodes"]}
        suchdol = [
            node for node in kut_tree["nodes"]
            if node["kind"] == "settlement"
            and any("suchdol" in str(value).lower() or "sukdol" in str(value).lower() for value in node["name"].values())
        ]
        print("Sukdol acceptance tree (maximum 5 structural levels):")
        if not suchdol:
            errors.append("Sukdol settlement node not found")
            print(" - NULL: settlement not found")
        else:
            descendants = set()
            queue = list(suchdol[0]["children"])
            while queue:
                current = queue.pop()
                descendants.add(current)
                queue.extend(nodes[current]["children"])
            main_fortresses = [
                nodes[node_id] for node_id in descendants
                if nodes[node_id]["kind"] == "castle"
                and (nodes[node_id]["name"].get("raw") or "").lower() in {"fortress", "ksuc_fortress_sh"}
            ]
            settlement = suchdol[0]
            print(
                f"- {display_name(settlement)} ({settlement['kind']}, "
                f"parent_source={settlement['parent_source']}, own={settlement['own']} "
                f"totals={settlement['totals']})"
            )
            for fortress in main_fortresses:
                print_subtree(fortress["id"], nodes, depth=1, maximum_depth=5)
            if len(main_fortresses) != 1:
                errors.append(f"Sukdol main fortress count is {len(main_fortresses)}, expected 1")
            elif not main_fortresses[0]["children"]:
                errors.append("Sukdol main fortress has no children")

    print("Overlay/merge/exterior/unresolved report:")
    for level, tree in trees.items():
        counts = tree["counts"]
        unresolved_doors = [door for door in tree["doors"] if door.get("reason")]
        unresolved_items = [item for item in tree["items"] if item.get("reason")]
        examples = [door["id"] for door in unresolved_doors[:3]]
        print(
            f" - {level}: overlays={counts['overlays_by_kind']}; merges={len(counts['castle_site_merges'])}; "
            f"settlement exterior items/doors={counts['items_on_settlement_exterior']}/"
            f"{counts['doors_on_settlement_exterior']}; unresolved doors={len(unresolved_doors)} {examples}; "
            f"unresolved items={len(unresolved_items)} {[item['id'] for item in unresolved_items[:3]]}"
        )

    if errors:
        print(f"FAILED with {len(errors)} error(s):")
        for error in errors[:100]:
            print(" -", error)
        return 1
    print("PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--catalog-directory", type=Path, default=Path("out/kcd2-area-catalog"))
    args = parser.parse_args()
    return validate(args.catalog_directory, args.fixture)


if __name__ == "__main__":
    raise SystemExit(main())
