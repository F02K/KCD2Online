import math
import struct

from tools.export_area_catalog import (
    access_from_labels,
    address_from_site_paths,
    area_kind,
    area_is_excluded_from_tree,
    asset_from_name,
    geometry_bbox,
    guid_to_engine_id,
    guid_to_u64,
    parse_triggerareas,
    point_in_polygon,
    transform_point,
    tree_floor_info,
    tree_overlay_kind,
    tree_site_kind,
    u64_to_guid,
)


def test_entity_guid_round_trip():
    guid = "3e05a7c3-dcda-0e46"
    assert u64_to_guid(guid_to_u64(guid)) == guid
    assert guid_to_engine_id(guid) == "0e46dcda3e05a7c3"


def test_parse_triggerareas_v3():
    guid = guid_to_u64("a774c00f-57a7-45bd")
    points = [(1.0, 2.0, 3.0), (4.0, 5.0, 6.0), (7.0, 8.0, 9.0)]
    data = struct.pack("<IIIQfI", 3, 0x12345678, 1, guid, 4.5, len(points))
    data += b"".join(struct.pack("<fff", *point) for point in points)
    header, rows = parse_triggerareas(data)
    assert header == {"version": 3, "header_value_hex": "12345678", "record_count": 1}
    assert rows[0]["guid"] == "a774c00f-57a7-45bd"
    assert rows[0]["height"] == 4.5
    assert rows[0]["polygon_world"] == [list(point) for point in points]


def test_transform_point_wxyz_quaternion():
    half = math.sqrt(0.5)
    transformed = transform_point([1.0, 0.0, 0.0], [10.0, 20.0, 30.0], [half, 0.0, 0.0, half], [2.0, 2.0, 2.0])
    assert transformed == [10.0, 22.0, 30.0]


def test_point_in_polygon():
    square = [[0.0, 0.0, 0.0], [10.0, 0.0, 0.0], [10.0, 10.0, 0.0], [0.0, 10.0, 0.0]]
    assert point_in_polygon([5.0, 5.0, 0.0], square)
    assert not point_in_polygon([15.0, 5.0, 0.0], square)


def test_engine_bounds_extrude_from_entity_origin():
    points = [[0.0, 0.0, 10.0], [1.0, 0.0, 14.0], [0.0, 1.0, 12.0]]
    assert geometry_bbox(points, 8.0, 14.0) == {
        "min": [0.0, 0.0, 10.0],
        "max": [1.0, 1.0, 22.0],
    }


def test_asset_and_strict_address_parsing():
    name = "SmartArea64[structures/living/houses/house_1s_3r_c6_87ad7113-cbfa-4b68-ba85-bafef57f3143]"
    assert asset_from_name(name) == "structures/living/houses/house_1s_3r_c6"
    assert address_from_site_paths(["main/kkut/city/tarmark/06_baker"]) == ("tarmark", 6, "baker")
    assert address_from_site_paths(["main/krat/village/6_home"]) == (None, None, None)


def test_place_tree_classification_helpers():
    assert access_from_labels(["interior", "private", "personal"]) == "private"
    assert tree_site_kind("fortress") == ("castle", "site_tree")
    assert tree_site_kind("series_courtyardhouses") == ("courtyard", "site_tree")
    assert tree_site_kind("hradecka") is None
    assert tree_floor_info("structures/living/houses/house_2s_9r_c") == {
        "count": 2,
        "source": "prefab",
    }


def test_place_tree_area_exclusion():
    base = {"entity_class": "TriggerArea", "editor_layer": "Main/test", "name": "usable"}
    assert not area_is_excluded_from_tree(base)
    assert area_is_excluded_from_tree({**base, "entity_class": "VisibilityArea"})
    assert area_is_excluded_from_tree({**base, "name": "cin_test_area"})


def test_place_tree_v2_overlay_rules_are_narrow():
    base = {
        "entity_class": "TriggerArea",
        "editor_layer": "main/test",
        "site_path": "main/test",
        "labels": [],
    }
    assert tree_overlay_kind({**base, "name": "SmartArea16_roof[structures/living/houses/house_1s_4r_b3]"}) == (
        "roof", "name:*_roof*",
    )
    assert tree_overlay_kind({**base, "name": "WH_TriggerArea5[structures/living/houses/small_roof_to_ground_house_c2]"}) is None
    assert tree_overlay_kind({**base, "name": "ksuc_interiorFortressArea_3", "labels": ["dogForbidden", "interior"]}) is None


def test_place_tree_v2_castle_courtyard_precedence():
    area = {
        "name": "ksuc_courtFortressArea_1",
        "site_path": "main/ksuc_suchdol/fortress",
        "labels": ["castle", "settlement"],
        "entity_class": "TriggerArea",
        "links": [],
        "relations": {"member_of_union_guids": []},
    }
    assert area_kind(area, False) == ("courtyard", "asset")
