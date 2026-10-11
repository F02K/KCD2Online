#!/usr/bin/env python3
"""Validate a property-context catalog against a recorded KCD2 engine dump."""

from __future__ import annotations

import argparse
import json
from collections import Counter
from pathlib import Path
from typing import Any


def vector(value: Any) -> list[float]:
    if isinstance(value, dict):
        return [float(value[axis]) for axis in ("x", "y", "z")]
    return [float(component) for component in value]


def expected_counts(properties: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "properties": len(properties),
        "by_origin": {
            origin: sum(row["origin"] == origin for row in properties)
            for origin in ("door_link", "asset_name", "hand_named_group")
        },
        "with_settlement": sum(row["settlement"] is not None for row in properties),
        "with_district_and_number": sum(
            row["district"] is not None and row["number"] is not None for row in properties
        ),
        "with_trade_layer": sum(row["trade_layer"] is not None for row in properties),
        "with_features": sum(bool(row["features"]) for row in properties),
    }


def validate(catalog_directory: Path, fixture_path: Path, sample_size: int = 10) -> int:
    fixture = json.loads(fixture_path.read_text(encoding="utf-8"))
    level = str(fixture.get("level") or "kutnohorsko").lower()
    area_catalog = json.loads((catalog_directory / f"areas_{level}.json").read_text(encoding="utf-8"))
    property_catalog = json.loads((catalog_directory / f"properties_{level}.json").read_text(encoding="utf-8"))
    manifest = json.loads((catalog_directory / "manifest.json").read_text(encoding="utf-8"))

    errors: list[str] = []
    catalog_areas = {row["engine_id"]: row for row in area_catalog["areas"]}
    matched = 0
    for expected in fixture["areas"]:
        actual = catalog_areas.get(expected["id"])
        if actual is None:
            errors.append(f"missing engine area {expected['id']} ({expected.get('name', '')})")
            continue
        matched += 1
        actual_bounds = (actual.get("geometry") or {}).get("bounding_box")
        if actual_bounds is None:
            errors.append(f"area {expected['id']} has no catalog bounds")
            continue
        for side in ("min", "max"):
            expected_vector = vector(expected["bounds"][side])
            actual_vector = vector(actual_bounds[side])
            deviations = [abs(left - right) for left, right in zip(expected_vector, actual_vector)]
            if max(deviations) > 2.0:
                errors.append(
                    f"area {expected['id']} {side} bounds deviate by {max(deviations):.3f} m"
                )
    print(f"Engine areas: {matched}/{len(fixture['areas'])} matched")

    properties = property_catalog["properties"]
    by_area_id = {
        area_id: row
        for row in properties
        for area_id in row["area_ids"]
    }
    linked_areas = [row for row in fixture["areas"] if row.get("linked")]
    linked_matched = 0
    linked_main = 0
    for expected in linked_areas:
        actual = by_area_id.get(expected["id"])
        if actual is None:
            errors.append(f"no property for linked area {expected['id']}")
            continue
        if actual["area_id"] == expected["id"]:
            linked_main += 1
        if actual["origin"] != "door_link":
            errors.append(f"linked area {expected['id']} has origin {actual['origin']!r}")
            continue
        expected_doors = sorted((door["guid"], door["link"]) for door in expected["linked"])
        actual_doors = sorted((door["door_id"], door["kind"]) for door in actual["doors"])
        if expected_doors != actual_doors:
            errors.append(
                f"linked area {expected['id']} doors differ: expected={expected_doors!r}, actual={actual_doors!r}"
            )
            continue
        linked_matched += 1
    print(
        f"Linked areas: {linked_matched}/{len(linked_areas)} matched "
        f"({linked_main} main areas; repeated-door components resolve through area_ids)"
    )

    for field, nested in (("area_ids", None), ("doors", "door_id")):
        values = []
        for row in properties:
            values.extend(item[nested] if nested else item for item in row[field])
        duplicates = [value for value, count in Counter(values).items() if count > 1]
        if duplicates:
            errors.append(f"duplicate {field}: {duplicates[:20]!r}")
        print(f"Unique {field}: {len(values) - len(duplicates)}/{len(values)}")

    calculated = expected_counts(properties)
    if property_catalog.get("counts") != calculated:
        errors.append(
            f"property file counts differ: stored={property_catalog.get('counts')!r}, calculated={calculated!r}"
        )
    manifest_level = next(
        (row for row in manifest["levels"] if row.get("directory") == level),
        None,
    )
    if manifest_level is None:
        errors.append(f"manifest has no level {level}")
    else:
        if manifest_level.get("properties_file") != f"properties_{level}.json":
            errors.append(f"manifest property filename is wrong for {level}")
        if manifest_level.get("property_counts") != calculated:
            errors.append(
                f"manifest property counts differ: stored={manifest_level.get('property_counts')!r}, calculated={calculated!r}"
            )
    print("Property counts:", json.dumps(calculated, ensure_ascii=False, sort_keys=True))

    print("Visual samples:")
    for origin in ("door_link", "asset_name", "hand_named_group"):
        samples = [row for row in properties if row["origin"] == origin][:sample_size]
        print(f"  {origin} ({len(samples)} shown):")
        for row in samples:
            print(
                "   ",
                json.dumps(
                    {
                        "area_id": row["area_id"],
                        "name": row["name"],
                        "site_path": row["site_path"],
                        "doors": len(row["doors"]),
                        "features": len(row["features"]),
                    },
                    ensure_ascii=False,
                    sort_keys=True,
                ),
            )

    if errors:
        print(f"FAILED with {len(errors)} error(s):")
        for error in errors[:100]:
            print(" -", error)
        if len(errors) > 100:
            print(f" - ... {len(errors) - 100} more")
        return 1
    print("PASS")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path)
    parser.add_argument(
        "--catalog-directory",
        type=Path,
        default=Path("out/kcd2-area-catalog"),
    )
    parser.add_argument("--sample-size", type=int, default=10)
    args = parser.parse_args()
    return validate(args.catalog_directory, args.fixture, args.sample_size)


if __name__ == "__main__":
    raise SystemExit(main())
