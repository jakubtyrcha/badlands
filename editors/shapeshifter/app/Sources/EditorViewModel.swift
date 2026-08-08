import SwiftUI
import AppKit
import ShapeshifterCore

/// The editing modes. Camera is deliberately **not** one of them: it is what
/// empty space does, in every mode, so it never has to be travelled to. Select
/// is gone too — Modify-with-nothing-selected already behaved exactly like it,
/// so the two collapsed into `.edit`.
enum EditorMode: CaseIterable, Hashable {
    case edit, spawn
}

/// Owns the core `Editor` and the mode/gesture state machine, and is the single
/// place raw input from the viewport gets routed and dispatched.
@Observable @MainActor
final class EditorViewModel {
    let editor = sq.Editor.create()!
    var mode: EditorMode = .edit

    /// How far the pointer must travel before a press stops being a click and
    /// becomes a drag. AppKit publishes no system drag threshold (unlike
    /// UIKit), so this is ours to own; it will want to grow for a stylus,
    /// which jitters far more than a trackpad tap.
    private static let dragThresholdPts: CGFloat = 4

    /// Spawn tool options, bound by SpawnOptionsBar; read on a `.spawn` click.
    var spawnShape: sq.Shape = .Cube
    var spawnOp: sq.Op = .Add

    /// Read-only mirrors of core's selection state (core owns the truth;
    /// these exist so SwiftUI views have something `@Observable` to read).
    var selectedNodeID: Int32? = nil
    var selectedNodeName: String? = nil

    /// True while a gizmo drag is running, so the radial menu can get out of
    /// the way of the gesture it started.
    var isDragging = false

    /// Radial-menu anchor: the selected node's projected viewport position, or
    /// nil when there's nothing to project onto.
    var radialAnchor: CGPoint? = nil

    /// Edit-menu mirrors. Core owns the one undo stack; these exist only so the
    /// menu has something `@Observable` to read, refreshed after every edit —
    /// the same pattern `refreshOverlayState` already follows.
    var canUndo = false
    var canRedo = false
    var undoTitle = "Undo"
    var redoTitle = "Redo"

    /// Which label a gizmo drag gets, from the handle it grabbed, so the Edit
    /// menu says what actually happened rather than a generic "Edit".
    private static func dragLabel(_ hit: sq.GizmoHit) -> String {
        if hit.slot == .Shape { return "Scale" }
        switch hit.handle {
        case .RingU, .RingV, .RingN: return "Rotate"
        default: return "Move"
        }
    }
    /// Mirrors `editor.nodeOp(selectedNodeID)`; nil exactly when
    /// `selectedNodeID` is nil.
    var selectedNodeOp: sq.Op? = nil

    /// The selected shape's profile parameter and its spec, or nil when there
    /// is no selection OR the selected shape has no parameter — those are the
    /// same answer to the radial menu ("no dial to draw"), so they collapse
    /// into one optional rather than needing the view to check `has_param`.
    var selectedNodeParamSpec: sq.ShapeParamSpec? = nil
    var selectedNodeParam: Float? = nil
    /// What the selected shape is, so the dial can say what it adjusts.
    var selectedNodeShape: sq.Shape? = nil

    /// What the current press is doing.
    ///
    /// A press starts `.pending` because a click and a drag are the same event
    /// until the pointer moves: below the threshold it keeps its mode meaning
    /// (select, spawn), past it the camera takes over. `.manipulating` is the
    /// one case decided at mouse-down, because `editor.beginDrag` has to
    /// capture the gizmo frame at the press position, not wherever the pointer
    /// has wandered to by the time the threshold is crossed.
    private enum PointerGesture {
        case pending(down: CGPoint, camera: sq.CameraGesture)
        case manipulating
        case camera(down: CGPoint)
    }
    private var pointer: PointerGesture? = nil

    /// Cumulative scroll/pinch travel, and whether a phased gesture is open.
    /// Trackpads report `.began/.changed/.ended`; a legacy wheel reports no
    /// phase at all, so there each event becomes its own complete gesture.
    private var scrollTotal: CGPoint = .zero
    private var scrollActive = false
    private var magnifyTotal: CGFloat = 0
    private var magnifyActive = false

    /// Single entry point for every mode change (buttons, keys, future
    /// shortcuts) — keep it that way so later work has one place to hook
    /// mode-transition side effects.
    func setMode(_ m: EditorMode) {
        // A mode key can fire mid-gesture. Without this the eventual
        // mouseUp would fail the mode guard and never call endDrag(), leaving
        // core's drag state pinned to whatever node was selected when the
        // gesture began. Core also guards defensively (updateDrag no-ops if
        // the selection no longer matches the captured node), but the gesture
        // should be cleanly ended here regardless.
        //
        // The INTERACTION has to be closed for the same reason, and the cost of
        // forgetting is worse than a stale drag. `pointer` is cleared just
        // below, so the eventual mouseUp lands in `case nil` and the matching
        // endInteraction never arrives — leaving History::depth_ pinned above
        // zero, where every later begin/end pair merely nests and NO undo entry
        // is ever pushed again. Silent, and permanent until a ⌘Z force-closes
        // it and commits the whole accumulated blob as one step.
        if isDragging {
            editor.endDrag()
            endInteraction()
            isDragging = false
        }
        pointer = nil
        mode = m
        syncGizmo()
        if m != .edit {
            // Belt to core's suspenders: core self-clears hover on selection
            // loss / gizmo hide / deletion, but a mode switch with the cursor
            // parked on a handle would otherwise leave the highlight lit.
            editor.clearGizmoHover()
        }
    }

    /// Centralizes gizmo visibility. Call after every mode or selection change.
    ///
    /// There is no kind to select any more: a selected node shows both of its
    /// manipulators at once, and which one you get is decided by the handle you
    /// grab. That is what lets one rule cover the whole viewport — core returns
    /// false from `beginDrag` off-handle, and the app hands the press to the
    /// camera.
    private func syncGizmo() {
        editor.setGizmoVisible(mode == .edit && selectedNodeID != nil)
    }

    // MARK: - Raw input, called by the viewport.

    /// Which camera verb a modifier chord asks for. Held modifiers are read
    /// once, at mouse-down, and kept for the whole gesture: sampling them
    /// per-event would let a mid-drag ⌥ press silently turn an orbit into a pan.
    private func cameraGesture(for modifiers: NSEvent.ModifierFlags) -> sq.CameraGesture {
        if modifiers.contains(.command) { return .Dolly }
        if modifiers.contains(.option) { return .Pan }
        return .Orbit
    }

    func handleMouseDown(_ p: CGPoint, modifiers: NSEvent.ModifierFlags) {
        let gesture = cameraGesture(for: modifiers)

        // A modifier chord is an explicit request for the camera, so the gizmo
        // is not even consulted — otherwise ⌥-dragging off a handle would be
        // the one place the chord didn't work.
        if gesture == .Orbit, mode == .edit, selectedNodeID != nil,
           editor.beginDrag(Float(p.x), Float(p.y)) {
            // AFTER beginDrag, deliberately. That call captures gizmo state and
            // mutates the document not at all, so the interaction still
            // brackets every edit — and only now do we know which handle was
            // grabbed, which is what the entry gets named after.
            beginInteraction(Self.dragLabel(editor.activeDragHandle()))
            pointer = .manipulating
            isDragging = true
            return
        }
        pointer = .pending(down: p, camera: gesture)
    }

    func handleMouseDragged(_ p: CGPoint) {
        switch pointer {
        case .pending(let down, let gesture):
            guard hypot(p.x - down.x, p.y - down.y) >= Self.dragThresholdPts else { return }
            // Anchored at the mouse-DOWN point, not the current one: that is
            // what makes "point at the feature and drag" rotate around the
            // thing you aimed at rather than wherever you have dragged to.
            editor.beginCameraGesture(gesture, Float(down.x), Float(down.y))
            pointer = .camera(down: down)
            updateCamera(from: down, to: p)

        case .camera(let down):
            updateCamera(from: down, to: p)

        case .manipulating:
            editor.updateDrag(Float(p.x), Float(p.y))
            refreshOverlayState()

        case nil:
            break
        }
    }

    func handleMouseUp(_ p: CGPoint) {
        switch pointer {
        case .pending(let down, _):
            performClick(at: down) // never crossed the threshold: it was a click

        case .camera:
            editor.endCameraGesture()

        case .manipulating:
            editor.endDrag()
            endInteraction() // the whole gesture becomes ONE undo entry
            // endDrag cleared the (stale) pre-drag hover; re-derive it from
            // where the mouse actually is, so a handle still under the cursor
            // stays lit without waiting for the next move.
            editor.updateGizmoHover(Float(p.x), Float(p.y))
            isDragging = false

        case nil:
            break
        }
        pointer = nil
    }

    private func updateCamera(from down: CGPoint, to p: CGPoint) {
        // Cumulative from the press, which is the contract core expects.
        editor.updateCameraGesture(Float(p.x - down.x), Float(p.y - down.y))
        refreshOverlayState()
    }

    /// A press that never became a drag keeps its mode's meaning.
    private func performClick(at p: CGPoint) {
        switch mode {
        case .edit:
            let r = editor.pick(Float(p.x), Float(p.y))
            editor.select(r.node_id) // miss returns kInvalidNode -> clears
            refreshSelectionMirrors()
            syncGizmo()
        case .spawn:
            // A click that spawns is a gesture like any other, so it is
            // bracketed like any other. The rule has no exceptions: if it
            // changes the document, it is inside an interaction.
            beginInteraction("Spawn")
            let s = editor.spawn(spawnShape, spawnOp, Float(p.x), Float(p.y))
            endInteraction()
            guard s.node_id != sq.kInvalidNode else { return } // zero-size viewport guard in core
            // Selection was already made by core (Editor::spawn calls select()
            // internally); the mirrors + mode switch are the VM's job.
            refreshSelectionMirrors()
            setMode(.edit)
        }
    }

    func handleMouseMoved(_ p: CGPoint) {
        // The dot predicts what an ORBIT will rotate around, and orbit works in
        // every mode, so it updates in every mode. Gating it on `.edit` (as an
        // earlier draft did) also left it frozen at its last Edit-mode position
        // for as long as you stayed in Spawn, since nothing then refreshed or
        // cleared it.
        //
        // Two analytic raycasts per move (gizmo handles, then the scene). Both
        // are ray-primitive solves over the node list, not raymarches -- see
        // core/src/picking.cpp -- so this is a handful of flops per node.
        editor.updateFocusPreview(Float(p.x), Float(p.y))
        guard mode == .edit else { return }
        editor.updateGizmoHover(Float(p.x), Float(p.y))
    }

    func handleMouseExited() {
        editor.clearGizmoHover()
        editor.clearFocusPreview()
    }

    /// Two-finger scroll pans. Phases let this be a real begin/update/end
    /// gesture on a trackpad; a legacy wheel reports none, so there each event
    /// is a complete one-shot gesture through the same contract.
    func handleScroll(dx: CGFloat, dy: CGFloat, at point: CGPoint,
                      phase: NSEvent.Phase, momentumPhase: NSEvent.Phase) {
        guard !phase.isEmpty || !momentumPhase.isEmpty else {
            oneShotCameraGesture(.Pan, at: point, dx: dx, dy: dy)
            return
        }

        // Momentum arrives as its own phase run after the fingers lift, so it
        // begins a fresh gesture rather than being dropped — otherwise flicking
        // to pan would stop dead the instant you let go.
        if phase.contains(.began) || momentumPhase.contains(.began) {
            scrollTotal = .zero
            editor.beginCameraGesture(.Pan, Float(point.x), Float(point.y))
            scrollActive = true
        }
        guard scrollActive else { return }

        scrollTotal.x += dx
        scrollTotal.y += dy
        editor.updateCameraGesture(Float(scrollTotal.x), Float(scrollTotal.y))
        refreshOverlayState()

        if phase.contains(.ended) || phase.contains(.cancelled)
            || momentumPhase.contains(.ended) || momentumPhase.contains(.cancelled) {
            editor.endCameraGesture()
            scrollActive = false
        }
    }

    /// Pinch dollies toward the cursor. `magnification` is per-event, so it is
    /// accumulated and handed to core as one cumulative value; core converts
    /// magnification to drag points itself, so no sensitivity constant lives here.
    func handleMagnify(_ magnification: CGFloat, at point: CGPoint, phase: NSEvent.Phase) {
        guard !phase.isEmpty else {
            let points = editor.dollyPointsForMagnification(Float(magnification))
            oneShotCameraGesture(.Dolly, at: point, dx: 0, dy: CGFloat(points))
            return
        }

        if phase.contains(.began) {
            magnifyTotal = 0
            editor.beginCameraGesture(.Dolly, Float(point.x), Float(point.y))
            magnifyActive = true
        }
        guard magnifyActive else { return }

        magnifyTotal += magnification
        editor.updateCameraGesture(0, editor.dollyPointsForMagnification(Float(magnifyTotal)))
        refreshOverlayState()

        if phase.contains(.ended) || phase.contains(.cancelled) {
            editor.endCameraGesture()
            magnifyActive = false
        }
    }

    private func oneShotCameraGesture(_ kind: sq.CameraGesture, at point: CGPoint,
                                      dx: CGFloat, dy: CGFloat) {
        editor.beginCameraGesture(kind, Float(point.x), Float(point.y))
        editor.updateCameraGesture(Float(dx), Float(dy))
        editor.endCameraGesture()
        refreshOverlayState()
    }

    /// Hook for MetalViewport's onSizeChange: a resize can move the selected
    /// node's projected anchor even though nothing else changed.
    func handleViewportSizeChange() {
        refreshOverlayState()
    }

    // MARK: - Radial menu actions
    //
    // No Move/Scale entries: both manipulators are live on every selection, so
    // there is nothing to arm. What is left is what the gizmos cannot express —
    // the node's CSG op, and deleting it.

    func radialToggleOp() {
        guard let id = selectedNodeID, let current = selectedNodeOp else { return }
        beginInteraction("Change Op")
        editor.setNodeOp(id, current == .Add ? .Subtract : .Add)
        endInteraction()
        refreshOverlayState()
    }

    /// Interaction passthroughs, so a view driving its own gesture (the shape
    /// dial) never has to touch `editor` directly.
    ///
    /// The dial is the case these exist for: `setSelectedShapeParam` fires on
    /// every mouse-move, and without a boundary each one would be its own undo
    /// step — turning the dial across its range would cost twenty of them.
    func beginInteraction(_ label: String) {
        editor.beginInteraction(label)
    }

    func endInteraction() {
        editor.endInteraction()
        refreshHistoryMirrors()
    }

    /// Drives the arc dial. `value` is raw — straight off the cursor's angle —
    /// because core clamps and snaps it; reading the mirror back afterwards is
    /// what makes the knob sit on a detent rather than wherever the cursor is.
    ///
    /// Deliberately does NOT set `isDragging`. That flag hides the whole radial
    /// menu so it cannot fight a gizmo drag, and this gesture IS the menu —
    /// raising it here would make the dial vanish from under the cursor
    /// turning it.
    func setSelectedShapeParam(_ value: Float) {
        guard let id = selectedNodeID else { return }
        editor.setNodeShapeParam(id, value)
        refreshOverlayState()
    }

    /// Deletes the selected node. Stays in `.edit`: there is no camera mode to
    /// fall back to any more, and none is needed. Undoable — ⌘Z brings the node
    /// back, still selected.
    func deleteSelected() {
        beginInteraction("Delete")
        editor.deleteSelectedNode()
        endInteraction()
        refreshSelectionMirrors()
        syncGizmo()
    }

    // MARK: - Undo / redo

    func undo() {
        editor.undo()
        refreshSelectionMirrors()
        syncGizmo()
    }

    func redo() {
        editor.redo()
        refreshSelectionMirrors()
        syncGizmo()
    }

    /// Returns true if the key was consumed (so the caller skips
    /// `super.keyDown`).
    func handleKeyDown(_ characters: String, modifiers: NSEvent.ModifierFlags) -> Bool {
        // ⌫ and ⌦ both delete, with no modifier — the Mac canvas-app convention
        // (Figma, Sketch, Keynote). Finder's ⌘⌫ is a Finder-specific safety
        // measure against deleting while typing, and this viewport has nothing
        // to type into.
        if characters == "\u{7F}" || characters.unicodeScalars.first?.value == UInt32(NSDeleteFunctionKey) {
            deleteSelected()
            return true
        }

        if modifiers.contains(.command) {
            switch characters.lowercased() {
            // ⇧⌘Z is the Mac redo. ⌘Y is a Windows convention and is unbound on
            // macOS — accepted silently below, but never shown in the menu.
            case "z":
                if modifiers.contains(.shift) { redo() } else { undo() }
                return true
            case "y": redo(); return true
            default: return false
            }
        }

        switch characters {
        case "1": setMode(.edit); return true
        case "2": setMode(.spawn); return true
        case "f", "F":
            editor.frameSelected()
            refreshOverlayState()
            return true
        default: return false
        }
    }

    // MARK: - Selection mirrors

    /// Single obvious refresh path: updates the selection mirrors (id/name)
    /// and then folds in `refreshOverlayState()`, since the radial menu's
    /// anchor and op both depend on what's selected.
    private func refreshSelectionMirrors() {
        let id = editor.selectedNode()
        if id == sq.kInvalidNode {
            selectedNodeID = nil
            selectedNodeName = nil
        } else {
            var buf = [CChar](repeating: 0, count: 64)
            editor.nodeName(id, &buf, 64)
            selectedNodeID = id
            // Deviation from `String(cString: buf)`: that overload (String from
            // a value-type [CChar] array) is deprecated on this toolchain.
            selectedNodeName = buf.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
        }
        refreshOverlayState()
        refreshHistoryMirrors()
    }

    /// Refreshes the Edit-menu mirrors from core. Called after every mutating
    /// action and by `refreshSelectionMirrors`, since undo and redo change the
    /// selection too.
    private func refreshHistoryMirrors() {
        canUndo = editor.canUndo()
        canRedo = editor.canRedo()
        undoTitle = Self.menuTitle("Undo", editor.undoLabel)
        redoTitle = Self.menuTitle("Redo", editor.redoLabel)
    }

    /// "Undo" alone when there is nothing pending, "Undo Move" when there is —
    /// the standard Mac Edit-menu shape.
    private static func menuTitle(_ verb: String, _ fill: (UnsafeMutablePointer<CChar>, Int32) -> Void) -> String {
        var buf = [CChar](repeating: 0, count: 64)
        fill(&buf, 64)
        let label = buf.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
        return label.isEmpty ? verb : "\(verb) \(label)"
    }

    // MARK: - Radial-menu overlay mirrors

    /// Refreshes `radialAnchor`/`selectedNodeOp` from core. Called by
    /// `refreshSelectionMirrors()` and directly after anything else that can
    /// move the anchor without changing the selection: camera gestures, drag
    /// updates, viewport resizes, and op toggles.
    private func refreshOverlayState() {
        let anchor = editor.projectSelectedAnchor()
        radialAnchor = anchor.visible ? CGPoint(x: CGFloat(anchor.x), y: CGFloat(anchor.y)) : nil
        selectedNodeOp = selectedNodeID.map { editor.nodeOp($0) }

        if let id = selectedNodeID {
            let spec = editor.nodeShapeParamSpec(id)
            selectedNodeShape = editor.nodeShape(id)
            selectedNodeParamSpec = spec.has_param ? spec : nil
            selectedNodeParam = spec.has_param ? editor.nodeShapeParam(id) : nil
        } else {
            selectedNodeShape = nil
            selectedNodeParamSpec = nil
            selectedNodeParam = nil
        }
    }
}
