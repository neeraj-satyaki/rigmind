// Zone based inference: polygon test, zone parsing, box test and the per-camera book (pipeline/zone_filter.h).
#include <iostream>

#include "pipeline/zone_filter.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
}
using rigmind::Zone;
using json = nlohmann::json;
} // namespace

int main() {
    // an L-shaped zone (concave) so the ray test is not just a box
    const std::vector<std::pair<float, float>> L = {{10, 10}, {60, 10}, {60, 40}, {40, 40}, {40, 80}, {10, 80}};
    check(rigmind::point_in_polygon(20, 20, L), "inside the tall arm");
    check(rigmind::point_in_polygon(50, 20, L), "inside the wide arm");
    check(!rigmind::point_in_polygon(50, 60, L), "in the notch of the L is outside");
    check(!rigmind::point_in_polygon(5, 5, L) && !rigmind::point_in_polygon(90, 90, L), "far outside");
    check(rigmind::point_in_polygon(10, 50, L), "on an edge counts as inside");
    check(rigmind::point_in_polygon(10, 10, L), "on a corner counts as inside");
    check(!rigmind::point_in_polygon(50, 50, {{0, 0}, {10, 10}}), "fewer than 3 points is never inside");

    // parsing
    const Zone ok = rigmind::parse_zone(json::parse(R"({"enabled":true,"points":[[10,10],[60,10],[60,60],[10,60]]})"));
    check(ok.enabled && ok.points.size() == 4, "a valid enabled zone");
    check(!rigmind::parse_zone(json::parse(R"({"enabled":false,"points":[[10,10],[60,10],[60,60]]})")).enabled, "disabled stays disabled");
    check(!rigmind::parse_zone(json::parse(R"({"enabled":true,"points":[[10,10],[60,10]]})")).enabled, "two points never enable a zone");
    check(!rigmind::parse_zone(json::parse(R"({"enabled":true,"points":[[10,10],[60,"x"],[60,60]]})")).enabled, "a bad point drops the whole zone");
    check(!rigmind::parse_zone(json::parse(R"({"enabled":true,"points":[[10,10],[60,10],[500,60]]})")).enabled, "a point far outside the frame is refused");
    check(!rigmind::parse_zone(json::parse(R"({"enabled":true})")).enabled && !rigmind::parse_zone(json("x")).enabled, "missing or non-object input");

    // boxes in pixels of a 1280x720 frame: zone = left half, upper two thirds (x 0-50 %, y 0-66 %)
    const Zone left = rigmind::parse_zone(json::parse(R"({"enabled":true,"points":[[0,0],[50,0],[50,66],[0,66]]})"));
    check(rigmind::box_in_zone(left, 100, 100, 300, 300, 1280, 720), "box centre inside");
    check(!rigmind::box_in_zone(left, 800, 100, 1000, 300, 1280, 720), "box centre in the right half");
    check(rigmind::box_in_zone(left, 500, 100, 700, 300, 1280, 720) == true, "centre at 600 px (46.9 %) is inside although the box reaches over the line");
    check(!rigmind::box_in_zone(left, 100, 600, 300, 700, 1280, 720), "box centre below the zone");
    check(rigmind::box_in_zone(Zone{}, 800, 600, 900, 700, 1280, 720), "no zone: everything counts");
    check(rigmind::box_in_zone(left, 800, 600, 900, 700, 0, 0), "an unknown frame size does not filter");

    // per camera
    rigmind::ZoneBook book;
    book.set("CAM-01", left);
    check(book.get("CAM-01").enabled && !book.get("CAM-02").enabled, "zones are per camera");
    book.set("CAM-01", Zone{});
    check(!book.get("CAM-01").enabled, "a zone can be switched off again");

    if (failures) return 1;
    std::cout << "zone_filter tests passed\n";
    return 0;
}
