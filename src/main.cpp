// hypredge — proportional pointer mapping between neighbouring monitors.
//
// Hyprland moves the pointer between monitors 1:1 in layout coordinates, so
// when two side-by-side monitors have different heights, part of the taller
// one's shared edge leads nowhere (a dead end) and crossings land at the same
// absolute height rather than the same relative height.
//
// Movement — hypredge hooks Pointer::CPointerManager::move and, after
// Hyprland has applied each relative pointer motion:
//   * if the pointer crossed onto a neighbouring monitor, it corrects the
//     position along the shared edge so it lands at the same RELATIVE spot
//     (e.g. 75% down the right monitor -> 75% down the left monitor);
//   * if the motion was swallowed at a dead end, it moves the pointer onto the
//     neighbour at the proportional spot.
//
// Drawing — while the pointer sits on a border, part of the cursor image
// overhangs onto the neighbour. Hyprland draws that part at the same absolute
// position; hypredge shifts it to the proportional spot, for both hardware
// cursors (getCursorPosForMonitor) and software cursors (the per-monitor box
// set by onCursorMoved / updateCursorBackend, redrawn via damageIfSoftware).
// These are private Hyprland internals, verified against Hyprland 0.56.2's
// source and binary: re-check by eye after every Hyprland update.
//
// The geometry lives in geometry.hpp (Hyprland-free, unit-tested); this file
// only wires it into the compositor.

#define WLR_USE_UNSTABLE

#include "geometry.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>
#include <hyprland/src/protocols/InputCapture.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

inline HANDLE PHANDLE = nullptr;

// Access to CPointerManager::m_monitorStates (private). Explicit template
// instantiation may name private members; the friend function hands out a
// reference. No layout tricks, no `#define private public`.
template <auto MEMBER>
struct SPrivateAccess {
    friend auto& pointerMonitorStates(Pointer::CPointerManager& pm) {
        return pm.*MEMBER;
    }
};
auto& pointerMonitorStates(Pointer::CPointerManager& pm);
template struct SPrivateAccess<&Pointer::CPointerManager::m_monitorStates>;

namespace {

    // Motion smaller than this (logical px) along an axis counts as "did not move".
    constexpr double BLOCKED_EPSILON = 0.02;

    using tMove       = void (*)(Pointer::CPointerManager*, const Vector2D&);
    using tCursorPos  = Vector2D (*)(Pointer::CPointerManager*, PHLMONITOR);
    using tVoidMethod = void (*)(Pointer::CPointerManager*);

    CFunctionHook* g_pMoveHook     = nullptr;
    tMove          g_pOriginalMove = nullptr;

    // Cursor drawing (private CPointerManager functions, found by name).
    //   getCursorPosForMonitor  hardware cursor position per monitor
    //   onCursorMoved           sets the software cursor box per monitor
    //   updateCursorBackend     sets it too (when falling back to software)
    //   damageIfSoftware        marks the software cursor area for redraw
    tCursorPos  g_pOriginalCursorPos      = nullptr;
    tVoidMethod g_pOriginalCursorMoved    = nullptr;
    tVoidMethod g_pOriginalUpdateBackend  = nullptr;
    tVoidMethod g_pOriginalDamageSoftware = nullptr;

    // Monitors the pointer can be on — the set Hyprland clamps the layout to.
    bool usable(const PHLMONITOR& mon) {
        return mon && mon->m_enabled && mon->m_output && !mon->isMirror();
    }

    // Snapshot of the usable monitors and their logical boxes (same order).
    struct SLayout {
        std::vector<PHLMONITOR> monitors;
        std::vector<CBox>       boxes;

        static SLayout current() {
            SLayout layout;
            for (const auto& mon : State::monitorState()->monitors()) {
                if (!usable(mon))
                    continue;
                layout.monitors.push_back(mon);
                layout.boxes.push_back(mon->logicalBox());
            }
            return layout;
        }

        std::optional<size_t> indexOf(const PHLMONITOR& mon) const {
            const auto IT = std::ranges::find(monitors, mon);
            if (IT == monitors.end())
                return std::nullopt;
            return static_cast<size_t>(IT - monitors.begin());
        }
    };

    // Index of the box containing `pos` (half-open, like pixel ownership), or
    // the nearest box if none contains it.
    std::optional<size_t> boxOf(const std::vector<CBox>& boxes, const Vector2D& pos) {
        std::optional<size_t> nearest;
        double                nearestDistance = 0.0;

        for (size_t i = 0; i < boxes.size(); ++i) {
            const CBox& B = boxes[i];
            if (pos.x >= B.x && pos.x < B.x + B.w && pos.y >= B.y && pos.y < B.y + B.h)
                return i;

            const double DISTANCE = B.closestPoint(pos).distanceSq(pos);
            if (!nearest || DISTANCE < nearestDistance) {
                nearest         = i;
                nearestDistance = DISTANCE;
            }
        }

        return nearest;
    }

    // cursor:hotspot_padding (default 0, Hyprland clamps it to 0..100). With
    // padding, Hyprland parks the pointer that far inside the layout edge, so
    // the dead-end trigger and the landing inset grow by it.
    double cursorPadding() {
        static auto PADDING = CConfigValue<Config::INTEGER>("cursor:hotspot_padding");
        return std::clamp(static_cast<double>(*PADDING), 0.0, 100.0);
    }

    void hkMove(Pointer::CPointerManager* self, const Vector2D& deltaLogical) {
        if (std::isnan(deltaLogical.x) || std::isnan(deltaLogical.y)) {
            g_pOriginalMove(self, deltaLogical);
            return;
        }

        const Vector2D BEFORE = self->position();
        g_pOriginalMove(self, deltaLogical);
        const Vector2D AFTER = self->position();

        // A client is steering the pointer (locked/confined pointer, e.g. games,
        // or an input capture session): leave it alone.
        if (g_pInputManager->isConstrained())
            return;
        if (NProtocols::inputCapture && NProtocols::inputCapture->isCaptured())
            return;

        const auto  LAYOUT = SLayout::current();
        const auto& boxes  = LAYOUT.boxes;
        if (boxes.size() < 2)
            return;

        const auto FROM = boxOf(boxes, BEFORE);
        const auto TO   = boxOf(boxes, AFTER);
        if (!FROM || !TO)
            return;

        const double PADDING = cursorPadding();
        const double INSET   = hypredge::LANDING_INSET + PADDING;

        std::optional<Vector2D> target;

        if (*FROM != *TO) {
            // Crossed onto a neighbour: land at the same relative spot.
            target = hypredge::crossingTarget(boxes, *FROM, *TO, AFTER, INSET);
        } else {
            // Same monitor: was the push swallowed at an edge? Keep only the
            // push along axes where Hyprland ate the motion.
            Vector2D push = AFTER;
            bool     any  = false;
            if (std::abs(deltaLogical.x) > BLOCKED_EPSILON && std::abs(AFTER.x - BEFORE.x) <= BLOCKED_EPSILON) {
                push.x += deltaLogical.x;
                any = true;
            }
            if (std::abs(deltaLogical.y) > BLOCKED_EPSILON && std::abs(AFTER.y - BEFORE.y) <= BLOCKED_EPSILON) {
                push.y += deltaLogical.y;
                any = true;
            }
            if (any)
                target = hypredge::deadEndTarget(boxes, *TO, AFTER, push, hypredge::EDGE_TRIGGER_EPSILON + PADDING, INSET);
        }

        if (target && target->distanceSq(AFTER) > BLOCKED_EPSILON * BLOCKED_EPSILON)
            self->warpTo(*target);
    }

    // Logical offset at which the cursor must be drawn on `mon` so the part of
    // the cursor image overhanging onto it sits at the proportional spot.
    // Rotated/flipped outputs are left alone (for hardware and software
    // cursors alike): their device coordinates would need the offset
    // transformed as well.
    Vector2D drawOffsetFor(const SLayout& layout, const Vector2D& pointer, const PHLMONITOR& mon) {
        if (!mon || mon->m_transform != WL_OUTPUT_TRANSFORM_NORMAL || layout.boxes.size() < 2)
            return {0.0, 0.0};

        const auto OTHER   = layout.indexOf(mon);
        const auto CURRENT = boxOf(layout.boxes, pointer);
        if (!OTHER || !CURRENT)
            return {0.0, 0.0};

        return hypredge::drawOffset(layout.boxes, *CURRENT, *OTHER, pointer);
    }

    // Hardware cursor: position on `mon` in device (physical) pixels.
    Vector2D hkGetCursorPosForMonitor(Pointer::CPointerManager* self, PHLMONITOR mon) {
        const Vector2D POS    = g_pOriginalCursorPos(self, mon);
        const Vector2D OFFSET = drawOffsetFor(SLayout::current(), self->position(), mon);
        return mon ? POS + OFFSET * mon->m_scale : POS;
    }

    // Software cursor: Hyprland draws it from each monitor's stored box, which
    // onCursorMoved and updateCursorBackend fill in (one of them with an
    // inlined helper we cannot hook). After either runs, store the shifted box
    // instead. Recomputed from scratch each time, so it never accumulates.
    void fixSoftwareBoxes(Pointer::CPointerManager* self) {
        const auto     LAYOUT  = SLayout::current();
        const Vector2D POINTER = self->position();
        const CBox     GLOBAL  = self->getCursorBoxGlobal();

        for (const auto& state : pointerMonitorStates(*self)) {
            if (!state)
                continue;
            const auto MON = state->monitor.lock();
            if (!MON)
                continue;

            const Vector2D OFFSET = drawOffsetFor(LAYOUT, POINTER, MON);
            CBox           box    = GLOBAL.copy().translate(-MON->m_position);
            box.x += OFFSET.x;
            box.y += OFFSET.y;
            state->box = box;
        }
    }

    void hkOnCursorMoved(Pointer::CPointerManager* self) {
        g_pOriginalCursorMoved(self);
        fixSoftwareBoxes(self);
    }

    void hkUpdateCursorBackend(Pointer::CPointerManager* self) {
        g_pOriginalUpdateBackend(self);
        fixSoftwareBoxes(self);
    }

    // Hyprland marks the UNshifted cursor area for redraw; also mark the
    // shifted one so the overhanging half is drawn and erased properly. Same
    // conditions as Hyprland's own damageIfSoftware.
    void hkDamageIfSoftware(Pointer::CPointerManager* self) {
        g_pOriginalDamageSoftware(self);

        const auto     LAYOUT  = SLayout::current();
        const Vector2D POINTER = self->position();
        const CBox     GLOBAL  = self->getCursorBoxGlobal().expand(4);

        for (const auto& state : pointerMonitorStates(*self)) {
            if (!state)
                continue;
            const auto MON = state->monitor.lock();
            if (!MON || !MON->m_output || MON->isMirror())
                continue;

            const bool SOFTWARE = state->softwareLocks > 0 || state->hardwareFailed || MON->shouldUseSoftwareCursors();
            if (!SOFTWARE || MON->shouldSkipScheduleFrameOnMouseEvent())
                continue;

            const Vector2D OFFSET = drawOffsetFor(LAYOUT, POINTER, MON);
            if (OFFSET.x == 0.0 && OFFSET.y == 0.0)
                continue;

            CBox shifted = GLOBAL.copy();
            shifted.x += OFFSET.x;
            shifted.y += OFFSET.y;
            if (!shifted.overlaps({MON->m_position, MON->m_size}))
                continue;

            MON->addDamage(shifted.translate(-MON->m_position).scale(MON->m_scale).round());
        }
    }

    // Find a CPointerManager member by name; nullptr if not exactly one match.
    void* findPointerManagerFn(const std::string& name) {
        void* found = nullptr;
        for (const auto& match : HyprlandAPI::findFunctionsByName(PHANDLE, name)) {
            if (match.demangled.find("Pointer::CPointerManager::" + name + "(") == std::string::npos)
                continue;
            if (found)
                return nullptr; // ambiguous
            found = match.address;
        }
        return found;
    }

    // Hook a cursor-drawing function; returns the installed hook or nullptr.
    // Failure is not fatal: proportional movement still works, only the
    // drawing correction is missing.
    template <typename T>
    CFunctionHook* hookDrawing(const std::string& name, void* replacement, T& original) {
        void* fn = findPointerManagerFn(name);
        if (!fn)
            return nullptr;
        auto* hook = HyprlandAPI::createFunctionHook(PHANDLE, fn, replacement);
        if (!hook || !hook->hook())
            return nullptr;
        original = (T)hook->m_original;
        if (!original) {
            hook->unhook(); // never leave a hook installed that would call nullptr
            return nullptr;
        }
        return hook;
    }

    // All or nothing, so hardware and software cursors stay consistent.
    bool hookAllDrawing() {
        std::vector<CFunctionHook*> installed;
        const auto                  ADD = [&](CFunctionHook* hook) {
            if (hook)
                installed.push_back(hook);
            return hook != nullptr;
        };

        const bool OK = ADD(hookDrawing("getCursorPosForMonitor", (void*)&hkGetCursorPosForMonitor, g_pOriginalCursorPos)) &&
            ADD(hookDrawing("onCursorMoved", (void*)&hkOnCursorMoved, g_pOriginalCursorMoved)) &&
            ADD(hookDrawing("updateCursorBackend", (void*)&hkUpdateCursorBackend, g_pOriginalUpdateBackend)) &&
            ADD(hookDrawing("damageIfSoftware", (void*)&hkDamageIfSoftware, g_pOriginalDamageSoftware));

        if (!OK) {
            for (auto* hook : installed)
                hook->unhook();
        }
        return OK;
    }

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // This plugin depends on the exact struct layouts and symbols of one
    // Hyprland build; refuse to run against any other.
    if (std::string_view{__hyprland_api_get_hash()} != std::string_view{__hyprland_api_get_client_hash()}) {
        HyprlandAPI::addNotification(PHANDLE, "[hypredge] Hyprland was updated — rebuild the plugin (make) and load it again", CHyprColor{1.0, 0.2, 0.2, 1.0}, 10000);
        throw std::runtime_error("hypredge: Hyprland version mismatch, rebuild needed");
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpmf-conversions"
    g_pMoveHook = HyprlandAPI::createFunctionHook(PHANDLE, (void*)&Pointer::CPointerManager::move, (void*)&hkMove);
#pragma GCC diagnostic pop

    if (g_pMoveHook && g_pMoveHook->hook())
        g_pOriginalMove = (tMove)g_pMoveHook->m_original;

    if (!g_pOriginalMove) {
        HyprlandAPI::addNotification(PHANDLE, "[hypredge] failed to hook pointer movement", CHyprColor{1.0, 0.2, 0.2, 1.0}, 10000);
        throw std::runtime_error("hypredge: failed to hook CPointerManager::move");
    }

    if (!hookAllDrawing()) {
        HyprlandAPI::addNotification(PHANDLE, "[hypredge] could not hook cursor drawing — movement works, but the cursor may be drawn at the old spot across edges",
                                     CHyprColor{1.0, 0.7, 0.2, 1.0}, 8000);
    }

    return {"hypredge", "Proportional pointer mapping between neighbouring monitors", "OceanProgramming", "0.3.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // Function hooks are owned by Hyprland's plugin system and removed on unload.
}
