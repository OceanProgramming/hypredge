// Pure geometry behind hypredge's proportional edge mapping.
//
// Nothing in here touches Hyprland: every function takes monitor boxes in
// logical (layout) coordinates and returns where the pointer should be. That
// keeps the math unit-testable on synthetic layouts (tests/geometry_test.cpp).
//
// The idea: when two monitors sit side by side (or stacked), map the WHOLE
// shared side of one onto the WHOLE side of the other by relative position.
// Leaving the right monitor 75% of the way down lands 75% of the way down the
// left monitor, and vice versa, so there are no dead ends and a round trip
// returns to the same spot.
//
// Where the mapping is not well defined (two or more monitors along the same
// side), the functions return nullopt and Hyprland's normal behavior applies.

#pragma once

#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Vector2D.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace hypredge {

    using Hyprutils::Math::CBox;
    using Hyprutils::Math::Vector2D;

    // Two monitor edges count as touching when they align within this many
    // logical pixels.
    inline constexpr double EDGE_SHARE_EPSILON = 0.5;
    // The pointer must sit within this many logical pixels of an edge to count
    // as pinned against it (callers widen it by cursor:hotspot_padding).
    inline constexpr double EDGE_TRIGGER_EPSILON = 0.5;
    // How far inside the target monitor the pointer lands (callers widen it by
    // cursor:hotspot_padding).
    inline constexpr double LANDING_INSET = 1.0;

    // Which side of a monitor another monitor touches.
    //   axis 0: left/right neighbor (crossing a vertical edge)
    //   axis 1: top/bottom neighbor (crossing a horizontal edge)
    //   dir +1: right/below, -1: left/above
    struct SSide {
        int  axis = 0;
        int  dir  = 0;
        bool operator==(const SSide&) const = default;
    };

    inline double lowAlong(const CBox& b, int axis) {
        return axis == 0 ? b.x : b.y;
    }
    inline double lenAlong(const CBox& b, int axis) {
        return axis == 0 ? b.w : b.h;
    }
    inline double coord(const Vector2D& v, int axis) {
        return axis == 0 ? v.x : v.y;
    }
    inline void setCoord(Vector2D& v, int axis, double value) {
        (axis == 0 ? v.x : v.y) = value;
    }

    // The side of `from` that `to` touches, if they share part of an edge.
    // Corner-only contact and gaps do not count.
    inline std::optional<SSide> sharedSide(const CBox& from, const CBox& to) {
        for (int axis = 0; axis < 2; ++axis) {
            const int    PERP    = 1 - axis;
            const double OVERLAP = std::min(lowAlong(from, PERP) + lenAlong(from, PERP), lowAlong(to, PERP) + lenAlong(to, PERP)) -
                std::max(lowAlong(from, PERP), lowAlong(to, PERP));
            if (OVERLAP <= 0.0)
                continue;

            if (std::abs(lowAlong(from, axis) + lenAlong(from, axis) - lowAlong(to, axis)) <= EDGE_SHARE_EPSILON)
                return SSide{axis, +1};
            if (std::abs(lowAlong(from, axis) - (lowAlong(to, axis) + lenAlong(to, axis))) <= EDGE_SHARE_EPSILON)
                return SSide{axis, -1};
        }
        return std::nullopt;
    }

    // Number of monitors in `all` (other than all[self]) touching all[self] on `side`.
    inline int neighborsOnSide(const std::vector<CBox>& all, size_t self, const SSide& side) {
        int count = 0;
        for (size_t i = 0; i < all.size(); ++i) {
            if (i != self && sharedSide(all[self], all[i]) == side)
                ++count;
        }
        return count;
    }

    // True when all[a] and all[b] are each other's only neighbor on the sides
    // they share, i.e. a 1:1 proportional mapping between them is well defined.
    inline std::optional<SSide> exclusivePair(const std::vector<CBox>& all, size_t a, size_t b) {
        const auto SIDE = sharedSide(all[a], all[b]);
        if (!SIDE)
            return std::nullopt;
        if (neighborsOnSide(all, a, *SIDE) != 1)
            return std::nullopt;
        if (neighborsOnSide(all, b, SSide{SIDE->axis, -SIDE->dir}) != 1)
            return std::nullopt;
        return SIDE;
    }

    // Map a perpendicular coordinate from `from`'s side onto `to`'s side by
    // relative position, kept `inset` inside `to`.
    inline double mapProportional(double p, const CBox& from, const CBox& to, int perp, double inset) {
        const double FROM_LOW = lowAlong(from, perp);
        const double FROM_LEN = lenAlong(from, perp);
        const double TO_LOW   = lowAlong(to, perp);
        const double TO_LEN   = lenAlong(to, perp);

        const double FRAC   = FROM_LEN > 0.0 ? std::clamp((p - FROM_LOW) / FROM_LEN, 0.0, 1.0) : 0.0;
        const double MAPPED = TO_LOW + FRAC * TO_LEN;

        const double PAD  = std::min(inset, TO_LEN / 2.0);
        const double LOW  = TO_LOW + PAD;
        const double HIGH = std::max(LOW, TO_LOW + TO_LEN - PAD);
        return std::clamp(MAPPED, LOW, HIGH);
    }

    // The pointer crossed from all[from] onto all[to] and Hyprland put it at
    // `pos`. Returns the proportionally corrected position, or nullopt to leave
    // the move alone.
    inline std::optional<Vector2D> crossingTarget(const std::vector<CBox>& all, size_t from, size_t to, const Vector2D& pos, double inset) {
        const auto SIDE = exclusivePair(all, from, to);
        if (!SIDE)
            return std::nullopt;

        const int PERP   = 1 - SIDE->axis;
        Vector2D  target = pos;
        setCoord(target, PERP, mapProportional(coord(pos, PERP), all[from], all[to], PERP, inset));
        return target;
    }

    // The pointer at `pos` on all[current] was pushed towards `desired` but
    // Hyprland kept it pinned against an edge (a dead end: no monitor directly
    // behind that spot). `desired - pos` must only be non-zero along axes where
    // the motion was swallowed. If a monitor exclusively shares the pushed
    // side, return the proportional landing point on it; otherwise nullopt.
    inline std::optional<Vector2D> deadEndTarget(const std::vector<CBox>& all, size_t current, const Vector2D& pos, const Vector2D& desired, double triggerEpsilon,
                                                 double inset) {
        const CBox&             CUR = all[current];
        std::optional<Vector2D> best;
        double                  bestDistance = 0.0;

        for (int axis = 0; axis < 2; ++axis) {
            const double PUSH = coord(desired, axis) - coord(pos, axis);
            if (std::abs(PUSH) <= 1e-6)
                continue;

            const int    DIR  = PUSH > 0 ? +1 : -1;
            const double EDGE = DIR > 0 ? lowAlong(CUR, axis) + lenAlong(CUR, axis) : lowAlong(CUR, axis);

            // Must be pinned against the edge being pushed towards. (The caller
            // only passes a push along axes where Hyprland swallowed the motion.)
            if (std::abs(coord(pos, axis) - EDGE) > triggerEpsilon)
                continue;

            for (size_t i = 0; i < all.size(); ++i) {
                if (i == current)
                    continue;
                const auto SIDE = exclusivePair(all, current, i);
                if (!SIDE || *SIDE != SSide{axis, DIR})
                    continue;

                const CBox&  TO   = all[i];
                const int    PERP = 1 - axis;
                const double PAD  = std::min(inset, lenAlong(TO, axis) / 2.0);

                Vector2D target;
                setCoord(target, axis, DIR > 0 ? lowAlong(TO, axis) + PAD : lowAlong(TO, axis) + lenAlong(TO, axis) - PAD);
                setCoord(target, PERP, mapProportional(coord(pos, PERP), CUR, TO, PERP, inset));

                const double DISTANCE = target.distanceSq(desired);
                if (!best || DISTANCE < bestDistance) {
                    best         = target;
                    bestDistance = DISTANCE;
                }
            }
        }

        return best;
    }

    // Cursor drawing: with the pointer at `pos` on all[current], how far (in
    // logical px) the cursor image drawn on all[other] must be shifted so the
    // part overhanging the shared edge appears at the proportional spot, not
    // at the same absolute height. Zero when the two monitors are not an
    // exclusive side-by-side pair.
    inline Vector2D drawOffset(const std::vector<CBox>& all, size_t current, size_t other, const Vector2D& pos) {
        if (current == other)
            return {0.0, 0.0};

        const auto SIDE = exclusivePair(all, current, other);
        if (!SIDE)
            return {0.0, 0.0};

        const int PERP   = 1 - SIDE->axis;
        Vector2D  offset = {0.0, 0.0};
        setCoord(offset, PERP, mapProportional(coord(pos, PERP), all[current], all[other], PERP, 0.0) - coord(pos, PERP));
        return offset;
    }

} // namespace hypredge
