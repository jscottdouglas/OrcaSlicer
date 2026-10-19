#include <catch2/catch_test_macros.hpp>
#include <string>

#include "slic3r/Utils/CrealityCFS.hpp"

using namespace Slic3r;

namespace {

// A slot table as boxsInfo leaves it: every slot of units 1 to 4 present, with
// the type the touchscreen or the RFID tag gave it, and no remaining length yet.
CfsSlotTable loaded_table()
{
    CfsSlotTable table;
    for (int unit = 1; unit <= 4; ++unit)
        for (int mid = 0; mid < 4; ++mid) {
            CfsSlot s;
            s.unit    = unit;
            s.slot    = mid;
            s.type    = "PLA";
            s.present = true;
            table.slots.push_back(s);
        }
    return table;
}

const CfsSlot& slot(const CfsSlotTable& table, int unit, int mid)
{
    const CfsSlot* s = table.get(unit, mid);
    REQUIRE(s != nullptr);
    return *s;
}

// Moonraker's box object as a K2 Plus reported it on 2026-10-19, trimmed to the
// three keys the merge reads. T3 holds three hand-set spools: a coded type, vender
// "unknown" and 0 m. T1 and T4 hold tagged spools the printer is not tracking
// (T1) and one it is (T4A, 50 m). T2A carries a full tag record in vender.
const char* const k2_box_2026_10_19 = R"({
    "map": {"T1A": "T3C"},
    "T1": {"material_type": ["-1", "0P1003", "-1", "0P1003"],
           "vender": ["none", "unknown", "none", "unknown"],
           "remain_len": ["0", "0", "0", "0"]},
    "T2": {"material_type": ["1P1003", "-1", "-1", "0P1003"],
           "vender": ["AB1240276A21P10030B1BEC60330000001000000", "none", "none", "unknown"],
           "remain_len": ["100", "0", "0", "0"]},
    "T3": {"material_type": ["000003", "000001", "000003", "-1"],
           "vender": ["unknown", "unknown", "unknown", "none"],
           "remain_len": ["0", "0", "0", "0"]},
    "T4": {"material_type": ["0P1003", "-1", "-1", "-1"],
           "vender": ["unknown", "none", "none", "none"],
           "remain_len": ["50", "0", "0", "0"]}
})";

} // namespace

TEST_CASE("A hand-set CFS spool reading 0 m has an unknown length, not an empty one", "[CFS][Creality]")
{
    CfsSlotTable table = loaded_table();
    REQUIRE(CrealityCFS::merge_box_object(k2_box_2026_10_19, table));

    // 3A, 3B, 3C: coded type, vender "unknown". Used to read 0.0 m and warn.
    CHECK(slot(table, 3, 0).remain_len < 0.0);
    CHECK(slot(table, 3, 1).remain_len < 0.0);
    CHECK(slot(table, 3, 2).remain_len < 0.0);
    // The same for a tagged spool the printer says it is not tracking.
    CHECK(slot(table, 1, 1).remain_len < 0.0);
    CHECK(slot(table, 2, 3).remain_len < 0.0);
    // A coded type is not a manual edit marker: only "unknown" in material_type is.
    CHECK_FALSE(slot(table, 3, 0).edited);
    CHECK(table.gcode_map.at("T1A") == "T3C");
}

TEST_CASE("A tracked CFS spool keeps its remaining length", "[CFS][Creality]")
{
    CfsSlotTable table = loaded_table();
    REQUIRE(CrealityCFS::merge_box_object(k2_box_2026_10_19, table));

    CHECK(slot(table, 2, 0).remain_len == 100.0);
    // vender "unknown" with a real figure: the figure stands.
    CHECK(slot(table, 4, 0).remain_len == 50.0);
    // An empty slot ("-1", vender "none") is not untagged: its 0 m is left alone.
    CHECK(slot(table, 3, 3).remain_len == 0.0);
}

TEST_CASE("An untagged CFS spool on older firmware still reads as unknown length", "[CFS][Creality]")
{
    CfsSlotTable table = loaded_table();
    REQUIRE(CrealityCFS::merge_box_object(R"({"T2": {"material_type": ["0P1003", "unknown"],
                                                     "remain_len": [0, 0]}})",
                                          table));

    CHECK(slot(table, 2, 1).remain_len < 0.0);
    CHECK(slot(table, 2, 1).edited);
    // No vender array at all and a tagged type: the 0 m stands.
    CHECK(slot(table, 2, 0).remain_len == 0.0);
}

TEST_CASE("A box object that is not JSON leaves the slot table alone", "[CFS][Creality]")
{
    CfsSlotTable table = loaded_table();
    CHECK_FALSE(CrealityCFS::merge_box_object("not json", table));
    CHECK(slot(table, 3, 0).remain_len < 0.0);
    CHECK(table.gcode_map.empty());
}
