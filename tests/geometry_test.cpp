// Synthetic-layout tests for hypredge's proportional mapping (no Hyprland needed).
//
//   make test
//
// All coordinates are doubles: Vector2D has both (int, int) and (double,
// double) constructors, so integer brace lists would be ambiguous.

#include "../src/geometry.hpp"

#include <cmath>
#include <cstdio>
#include <optional>

using namespace hypredge;

static int g_failures = 0;

static void expect(const char* name, const std::optional<Vector2D>& got, const std::optional<Vector2D>& want) {
    const auto SAME = [](const Vector2D& a, const Vector2D& b) { return std::abs(a.x - b.x) < 1e-6 && std::abs(a.y - b.y) < 1e-6; };

    if (got.has_value() == want.has_value() && (!got || SAME(*got, *want))) {
        std::printf("PASS  %s\n", name);
        return;
    }

    ++g_failures;
    std::printf("FAIL  %s\n", name);
    if (want)
        std::printf("      want (%.3f, %.3f)\n", want->x, want->y);
    else
        std::printf("      want (none)\n");
    if (got)
        std::printf("      got  (%.3f, %.3f)\n", got->x, got->y);
    else
        std::printf("      got  (none)\n");
}

int main() {
    const double T = EDGE_TRIGGER_EPSILON;
    const double I = LANDING_INSET;

    // ── The real desk: Dell U2415 1920x1200 left, Dell U2719D 2560x1440 right,
    //    both at y = 0 (top-aligned).
    const std::vector<CBox> DESK{CBox{{0.0, 0.0}, {1920.0, 1200.0}}, CBox{{1920.0, 0.0}, {2560.0, 1440.0}}};
    constexpr size_t        L = 0, R = 1;

    // Crossings: Hyprland already moved the pointer onto the other monitor at
    // the same absolute y; hypredge rescales y by relative height.
    expect("R->L halfway down (720 of 1440) lands halfway down L (600)", //
           crossingTarget(DESK, R, L, {1919.0, 720.0}, I), Vector2D{1919.0, 600.0});
    expect("R->L at the top stays at the top", //
           crossingTarget(DESK, R, L, {1919.0, 0.0}, I), Vector2D{1919.0, 1.0});
    expect("L->R halfway down (600 of 1200) lands halfway down R (720)", //
           crossingTarget(DESK, L, R, {1921.0, 600.0}, I), Vector2D{1921.0, 720.0});
    expect("L->R from the very bottom of L lands at the very bottom of R", //
           crossingTarget(DESK, L, R, {1921.0, 1199.0}, I), Vector2D{1921.0, 1199.0 * 1.2});

    // Round trip returns to the same spot.
    {
        const auto THERE = crossingTarget(DESK, L, R, {1921.0, 900.0}, I);
        const auto BACK  = THERE ? crossingTarget(DESK, R, L, {1919.0, THERE->y}, I) : std::nullopt;
        expect("round trip L->R->L returns to y = 900", BACK, Vector2D{1919.0, 900.0});
    }

    // Dead ends: the bottom 240 px of R's left edge have nothing beside them.
    expect("dead end: R pinned at y=1300 pushing left -> L at 1300*(1200/1440)", //
           deadEndTarget(DESK, R, {1920.0, 1300.0}, {1918.0, 1300.0}, T, I), Vector2D{1919.0, 1300.0 * 1200.0 / 1440.0});
    expect("dead end: R bottom corner pushing left -> bottom of L (kept 1 px inside)", //
           deadEndTarget(DESK, R, {1920.0, 1439.0}, {1917.0, 1439.0}, T, I), Vector2D{1919.0, 1199.0});
    expect("not at the edge: pushing left in the middle of R does nothing", //
           deadEndTarget(DESK, R, {3000.0, 1300.0}, {2998.0, 1300.0}, T, I), std::nullopt);
    expect("pinned at R's bottom edge pushing down: nothing below, nothing happens", //
           deadEndTarget(DESK, R, {2500.0, 1440.0}, {2500.0, 1442.0}, T, I), std::nullopt);
    expect("pinned at R's right edge pushing right: nothing there", //
           deadEndTarget(DESK, R, {4480.0, 500.0}, {4482.0, 500.0}, T, I), std::nullopt);
    expect("cursor padding 10 px: still triggers, lands 11 px inside", //
           deadEndTarget(DESK, R, {1930.0, 1300.0}, {1928.0, 1300.0}, T + 10.0, I + 10.0), Vector2D{1909.0, 1300.0 * 1200.0 / 1440.0});

    // ── Bottom-aligned variant (L at y = 240) works the same way.
    const std::vector<CBox> BOTTOM{CBox{{0.0, 240.0}, {1920.0, 1200.0}}, CBox{{1920.0, 0.0}, {2560.0, 1440.0}}};
    expect("bottom-aligned: R top (y=0) pushing left -> L top (y=241)", //
           deadEndTarget(BOTTOM, R, {1920.0, 0.0}, {1918.0, 0.0}, T, I), Vector2D{1919.0, 241.0});
    expect("bottom-aligned: R->L halfway lands halfway down L (840)", //
           crossingTarget(BOTTOM, R, L, {1919.0, 720.0}, I), Vector2D{1919.0, 840.0});

    // ── Stacked monitors: a vertical pair maps x proportionally too.
    const std::vector<CBox> STACK{CBox{{0.0, 0.0}, {2560.0, 1440.0}}, CBox{{0.0, 1440.0}, {1920.0, 1080.0}}};
    expect("stacked: top->bottom at x=1280 (50%) lands at x=960", //
           crossingTarget(STACK, 0, 1, {1280.0, 1441.0}, I), Vector2D{960.0, 1441.0});
    expect("stacked: dead end at top's bottom edge (x=2200) pushing down -> bottom monitor", //
           deadEndTarget(STACK, 0, {2200.0, 1440.0}, {2200.0, 1442.0}, T, I), Vector2D{2200.0 * 1920.0 / 2560.0, 1441.0});

    // ── Ambiguous: two monitors stacked along the same side -> no remapping.
    const std::vector<CBox> THREE{CBox{{0.0, 0.0}, {1000.0, 2000.0}}, CBox{{1000.0, 0.0}, {800.0, 900.0}}, CBox{{1000.0, 1000.0}, {800.0, 1000.0}}};
    expect("two neighbours on one side: crossing left untouched", //
           crossingTarget(THREE, 0, 1, {1001.0, 500.0}, I), std::nullopt);
    expect("two neighbours on one side: dead end left untouched", //
           deadEndTarget(THREE, 0, {1000.0, 950.0}, {1002.0, 950.0}, T, I), std::nullopt);

    // ── Layout defects stay hard walls.
    const std::vector<CBox> GAP{CBox{{0.0, 0.0}, {1000.0, 1000.0}}, CBox{{1002.0, 0.0}, {800.0, 600.0}}};
    expect("2 px gap between monitors: no transfer", //
           deadEndTarget(GAP, 0, {1000.0, 800.0}, {1002.0, 800.0}, T, I), std::nullopt);
    const std::vector<CBox> CORNER{CBox{{0.0, 0.0}, {1000.0, 1000.0}}, CBox{{1000.0, 1000.0}, {800.0, 800.0}}};
    expect("corner-only touch: no transfer", //
           deadEndTarget(CORNER, 0, {1000.0, 999.0}, {1002.0, 999.0}, T, I), std::nullopt);

    // ── Negative coordinates (monitor left of the origin).
    const std::vector<CBox> NEG{CBox{{-1920.0, 0.0}, {1920.0, 1200.0}}, CBox{{0.0, 0.0}, {2560.0, 1440.0}}};
    expect("negative coords: dead end pushing left maps proportionally", //
           deadEndTarget(NEG, 1, {0.0, 1440.0 * 0.9}, {-2.0, 1440.0 * 0.9}, T, I), Vector2D{-1.0, 1200.0 * 0.9});

    // ── Drawing the overhanging half of the cursor on the other monitor.
    expect("draw: pointer on L at y=600 -> overhang on R drawn 120 px lower (at 720)", //
           drawOffset(DESK, L, R, {1915.0, 600.0}), Vector2D{0.0, 120.0});
    expect("draw: pointer on R at y=720 -> overhang on L drawn 120 px higher (at 600)", //
           drawOffset(DESK, R, L, {1921.0, 720.0}), Vector2D{0.0, -120.0});
    expect("draw: at the top nothing shifts", //
           drawOffset(DESK, R, L, {1921.0, 0.0}), Vector2D{0.0, 0.0});
    expect("draw: the monitor the pointer is on is never shifted", //
           drawOffset(DESK, R, R, {1921.0, 720.0}), Vector2D{0.0, 0.0});
    expect("draw: stacked monitors shift along x", //
           drawOffset(STACK, 0, 1, {1280.0, 1438.0}), Vector2D{-320.0, 0.0});
    expect("draw: ambiguous side -> no shift", //
           drawOffset(THREE, 0, 1, {998.0, 500.0}), Vector2D{0.0, 0.0});

    if (g_failures > 0) {
        std::printf("\n%d test(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall tests passed\n");
    return 0;
}
