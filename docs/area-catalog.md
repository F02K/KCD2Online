# Native area JSON catalog

`tools/export_area_catalog.py` exports the game's area geometry and the data
needed to relate it to properties, buildings, rooms, jobs, POIs, and
settlements. It reads the installed retail data directly; it does not estimate
polygons from marker positions.

```powershell
python tools/export_area_catalog.py `
  "F:\Steam\steamapps\common\KingdomComeDeliverance2" `
  --output-directory out\kcd2-area-catalog
```

The result contains one JSON file for each native world plus `manifest.json`:

- `areas_trosecko.json` (level 2)
- `areas_kutnohorsko.json` (level 3)
- `areas_klaster.json` (level 4 / DLC monastery world)
- `properties_trosecko.json`
- `properties_kutnohorsko.json`
- `properties_klaster.json`
- `tree_trosecko.json`
- `tree_kutnohorsko.json`
- `tree_klaster.json`

`manifest.json` and every level file also record the installed game assembly,
release preset, and source-data versions. This makes a dump reproducible and
allows catalogs made from different game patches to be compared safely.

## Native data

Every `areas[]` entry retains the native entity GUID (both formatted and as an
unsigned 64-bit decimal string), the 16-character engine `engine_id`, entity
ID, class, name, editor layer, labels,
properties, links, and source XML files. `triggerareas.fubar` version 3 is
decoded as a stream of GUID, height, and world-space polygon vertices. XML
shape points are transformed from local to world coordinates with the entity's
position, WXYZ quaternion, and scale.

The geometry kinds are:

- `polygon_prism`: `polygon_world`, signed `height`, 2D area, and world AABB;
- `oriented_box`: local min/max, transformed world corners, and world AABB;
- `sphere`: transformed center, radius, and world AABB;
- `union`: native `area_union_member` relationships and their aggregate AABB.

An area entity can legitimately have no serialized geometry. Such records are
retained rather than invented and counted in `counts.areas_without_geometry`.

`locations[]` resolves `guidLocationId` from the native location table and
includes German and English display labels. This is where broad areas such as
Suchdol and Kuttenberg city are represented. `points_of_interest[]` contains
the native map POI objects, and `workstations_and_shops[]` contains entities
such as alchemy tables and smithies. Their `containing_area_guids` are ordered
from the smallest containing volume to the largest.

Every link additionally exposes `target_engine_id`. Location records contain
the engine ID of their location area and `member_area_ids` for areas contained
by that settlement.

## Derived relationships

The exporter keeps inference visibly separate under `derived`, `sites`, and
`semantic_tags`. The raw editor hierarchy becomes a site tree, so a consumer
can traverse settlement -> district -> property/building -> room. Native
`AreaUnion` membership is preserved separately and should be preferred for a
multi-building property whenever it is present.

The catalog deliberately retains every native area. For the requested subset
(jobs, shops, inns, homes, apartments, castles/fortresses and rooms, plus
settlements), filter on `derived.requested_scope`. More exact queries can use
tags such as `blacksmith`, `tailor`, `alchemy`, `inn`, `house`, `apartment`,
`castle`, `room`, `city`, or `settlement`.

```python
import json

catalog = json.load(open("out/kcd2-area-catalog/areas_kutnohorsko.json", encoding="utf-8"))
tailor_areas = [
    area for area in catalog["areas"]
    if "tailor" in area["derived"]["semantic_tags"]
]
```

Native facts and derived fields are summarized in the top-level `scope` block
of every output file.

The corrected `derived.semantic_tags` use only the area's own `site_path` and
features geometrically inside that area. The previous broader value remains
available as `derived.semantic_tags_legacy` for compatibility and comparison.

## Property context

`properties_<level>.json` uses schema `kcd2-property-context/v1` and omits
polygons and raw XML attributes. Properties are built in three deterministic
stages: exact native `crime_door` links, spatially matched private
`structures/...` areas, and supported hand-named area groups. An AreaUnion is
the main area for a building union; its members are listed in `area_ids`.

Each record contains settlement, strict city/village district address data,
asset name, doors, local features, floor information, aggregate bounds,
origin, and confidence. A door and an area can occur in at most one property.
Coordinates in these compact files are rounded to three decimals.

Some prefabs expose several component areas with the exact same door links.
Those components are one property: the lowest engine ID is `area_id`, and all
components remain discoverable through `area_ids`.

Validate the Kutnohorsko export against the recorded engine fixture with:

```powershell
python tools/validate_property_catalog.py `
  "E:\Github\boehmen-rp\resources\boehmen-property\tests\fixtures\kutnohorsko-dump.json" `
  --catalog-directory out\kcd2-area-catalog
```

The validator checks engine IDs, bounds, exact linked-door sets, uniqueness,
manifest counts, and prints ten visual samples for every property origin. It
returns a non-zero exit code on any mismatch.

## Place tree

`tree_<level>.json` uses schema `kcd2-place-tree/v2`. It adds a directly
navigable hierarchy of level, settlement, district, compound, castle,
courtyard, building, floor, room, outdoor, and other nodes without changing
the flat property files. Every non-excluded physical area occurs in exactly
one node. Non-place trigger overlays are stored in the owning structural
node's `overlays` array instead. The parent source records whether a
relationship comes from AreaUnion membership, the native site tree, geometry,
a location, a prefab floor, or an explicit unresolved container.

The tree also contains every native `AnimDoor` and primary house-relevant
entity: stashes/containers, bed triggers, alchemy and smithing stations,
grindstones, food-processing stations, shops, fireplaces, and ladders. Door
and item assignments select the smallest structural matching node and retain
their evidence source. `via_overlay` records a coincident technical overlay
without allowing it to own the entity. Native entities without usable world
context are retained under an explicit `unresolved_native_context` compound
and retain a reason; no place is invented for them. Complete ID/reason lists
are available in `limitations.unresolved_doors`, `unresolved_items`, and
`overlays_without_owner`.

Each structural node provides `own` direct counts and recursive `totals` for
doors, rooms, and item kinds. Buildings with a native `_1s_`, `_2s_`, etc.
prefab marker expose `floors.count`, `floors.source: "prefab"`, and one floor
node per declared storey. A floor range is derived only from native bounds or
room/door Z samples; missing ranges carry a reason.

Stealth, music, bird-takeoff, deterrent, ladder, dog/horse-control, roof, and
generic trigger areas never become place nodes. Their exact matching rules and
counts are published in each manifest level's `tree_overlay_patterns`.
Castle-outline and matching site-node merges are published in
`tree_castle_site_merges`. Animal-control classes, visibility areas, and
sequence/cutscene areas remain excluded and counted separately. Technical
sibling areas may share one compact node, but every physical engine ID remains
listed exactly once in `area_ids`.

Validate all three trees, the recorded Kutnohorsko areas/doors, Union
relationships, contents, and the Sukdol acceptance hierarchy with:

```powershell
python tools/validate_place_tree.py `
  "E:\Github\boehmen-rp\resources\boehmen-property\tests\fixtures\kutnohorsko-dump.json" `
  --catalog-directory out\kcd2-area-catalog
```
