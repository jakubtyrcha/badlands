import SwiftUI
import ShapeshifterCore

// Deviation from the brief's stub (`enum EditorMode: CaseIterable`): adding
// Hashable is required both for `mode == .camera` guards below and for
// ForEach(EditorMode.allCases, id: \.self) in ModeBar.
enum EditorMode: CaseIterable, Hashable {
    case select, spawn, modify, camera
}

/// Which of the two modify-mode drag gestures the radial menu currently
/// routes mouse input to.
enum RadialTool {
    case move, scale
}

/// Owns the core `Editor` and the 4-mode UI state machine, and is the single
/// place raw input from the viewport gets routed and dispatched by mode.
@Observable @MainActor
final class EditorViewModel {
    let editor = sq.Editor.create()!
    var mode: EditorMode = .select

    /// Spawn tool options, bound by SpawnOptionsBar; read by handleMouseDown
    /// in `.spawn` mode.
    var spawnShape: sq.Shape = .Cube
    var spawnOp: sq.Op = .Add

    /// Read-only mirrors of core's selection state (core owns the truth;
    /// these exist so SwiftUI views have something `@Observable` to read).
    var selectedNodeID: Int32? = nil
    var selectedNodeName: String? = nil

    /// True between a `.modify`-mode mouse-down that started a drag
    /// (`editor.beginDrag`/`editor.beginScale`) and the matching mouse-up.
    var isDragging = false

    /// Which radial-menu tool `.modify` mode's mouse gestures currently
    /// drive; flipped by the radial menu's Move/Scale buttons and reverted
    /// to `.move` when a scale gesture ends (see `radialSelectScale`/
    /// `handleMouseUp`).
    var activeRadialTool: RadialTool = .move
    /// Radial-menu anchor: the selected node's position projected to the
    /// viewport, or nil when there's nothing to project onto (hidden/
    /// invisible) — mirrors `editor.projectSelectedAnchor()`.
    var radialAnchor: CGPoint? = nil
    /// Mirrors `editor.nodeOp(selectedNodeID)`; nil exactly when
    /// `selectedNodeID` is nil.
    var selectedNodeOp: sq.Op? = nil

    /// Screen-space Y at the `.modify`-mode mouse-down that started the
    /// current scale gesture; `updateScale` is driven by the cumulative
    /// delta from this point (core's scale semantics are cumulative-from-
    /// start, not incremental — see `Editor::updateScale`).
    private var scaleDragStartY: CGFloat = 0

    /// Single entry point for every mode change (buttons, keys, future
    /// shortcuts) — keep it that way so later milestones have one place to
    /// hook mode-transition side effects (e.g. resetting spawn/gizmo state).
    func setMode(_ m: EditorMode) {
        // A mode key can fire mid-gesture (mouse button still down from a
        // .modify drag or scale). Without this, the eventual
        // mouseDragged/mouseUp would fail the `mode == .modify` guard and
        // never call endDrag()/endScale(), leaving core's drag/scale state
        // pinned to whatever node was selected when the gesture began — a
        // later gesture on a *different* node (after returning to .modify)
        // would then silently apply that stale plane/start state. Core also
        // guards both gestures defensively (Editor::updateDrag/updateScale
        // no-op if the selection no longer matches the captured node), but
        // the gesture should be cleanly ended here regardless.
        if mode == .modify, m != .modify, isDragging {
            switch activeRadialTool {
            case .move: editor.endDrag()
            case .scale:
                editor.endScale()
                // Mirror handleMouseUp's .scale arm: an aborted scale gesture
                // reverts to .move too, so the radial menu doesn't keep
                // showing "Scale" active (and the next modify-mode drag
                // doesn't silently start a fresh beginScale) after returning
                // to .modify with no radial-menu interaction to explain why.
                activeRadialTool = .move
            }
            isDragging = false
        }
        mode = m
        syncGizmo()
    }

    /// Centralizes the gizmo-visibility rule (core owns the gizmo's placement
    /// math; the VM only tells it whether to show). Call after every mode or
    /// selection change.
    private func syncGizmo() {
        editor.setGizmoVisible(mode == .modify && selectedNodeID != nil)
    }

    // MARK: - Raw input, called by the viewport.

    func handleMouseDown(_ p: CGPoint) {
        switch mode {
        case .select:
            let r = editor.pick(Float(p.x), Float(p.y))
            editor.select(r.node_id) // miss returns kInvalidNode -> clears
            refreshSelectionMirrors()
            syncGizmo()
        case .spawn:
            let s = editor.spawn(spawnShape, spawnOp, Float(p.x), Float(p.y))
            guard s.node_id != sq.kInvalidNode else { return } // zero-size viewport guard in core
            // Selection was already made by core (Editor::spawn calls
            // select() internally); the mirrors + mode switch are the VM's
            // job, same division of labor as .select's pick+select above.
            refreshSelectionMirrors()
            setMode(.modify) // selectedNodeID is already set above, so this also syncs the gizmo on
        case .modify:
            if selectedNodeID == nil {
                // Modify-awaiting-selection: behave exactly like select mode
                // until something is correctly clicked.
                let r = editor.pick(Float(p.x), Float(p.y))
                editor.select(r.node_id)
                refreshSelectionMirrors()
                syncGizmo()
            } else {
                switch activeRadialTool {
                case .move:
                    editor.beginDrag(Float(p.x), Float(p.y))
                case .scale:
                    editor.beginScale()
                    scaleDragStartY = p.y
                }
                isDragging = true
            }
        case .camera:
            break // no-op until later milestones
        }
    }

    func handleMouseDragged(_ p: CGPoint) {
        guard mode == .modify, isDragging else { return }
        switch activeRadialTool {
        case .move:
            editor.updateDrag(Float(p.x), Float(p.y))
        case .scale:
            editor.updateScale(Float(p.y - scaleDragStartY))
        }
        refreshOverlayState()
    }

    func handleMouseUp(_ p: CGPoint) {
        guard mode == .modify, isDragging else { return }
        switch activeRadialTool {
        case .move:
            editor.endDrag()
        case .scale:
            editor.endScale()
            activeRadialTool = .move
        }
        isDragging = false
    }

    func handleScroll(dx: CGFloat, dy: CGFloat, shiftHeld: Bool) {
        guard mode == .camera else { return }
        if shiftHeld {
            editor.cameraPan(Float(dx), Float(dy))
        } else {
            editor.cameraOrbit(Float(dx), Float(dy))
        }
        refreshOverlayState()
    }

    func handleMagnify(_ delta: CGFloat) {
        guard mode == .camera else { return }
        editor.cameraZoom(Float(delta))
        refreshOverlayState()
    }

    /// Hook for MetalViewport's onSizeChange: a viewport resize can move the
    /// selected node's projected screen anchor even though nothing else
    /// (selection, camera, node transform) changed.
    func handleViewportSizeChange() {
        refreshOverlayState()
    }

    // MARK: - Radial menu actions

    func radialSelectMove() {
        activeRadialTool = .move
    }

    func radialSelectScale() {
        activeRadialTool = .scale
    }

    func radialToggleOp() {
        guard let id = selectedNodeID, let current = selectedNodeOp else { return }
        editor.setNodeOp(id, current == .Add ? .Subtract : .Add)
        refreshOverlayState()
    }

    /// Returns true if the key was consumed (so the caller skips
    /// `super.keyDown`).
    func handleKeyDown(_ characters: String) -> Bool {
        switch characters {
        case "1": setMode(.select); return true
        case "2": setMode(.spawn); return true
        case "3": setMode(.modify); return true
        case "4": setMode(.camera); return true
        default: return false
        }
    }

    // MARK: - Selection mirrors

    /// Single obvious refresh path: updates the selection mirrors (id/name)
    /// and then, since the radial-menu overlay's anchor/op both depend on
    /// what's selected, folds in `refreshOverlayState()` too — callers never
    /// need to remember to call both.
    private func refreshSelectionMirrors() {
        let id = editor.selectedNode()
        // M4 deferred finding resolved: kInvalidNode moved to the interop
        // header (ShapeshifterCore.h) in this milestone specifically so
        // Swift could import it as a named sentinel instead of hardcoding -1.
        if id == sq.kInvalidNode {
            selectedNodeID = nil
            selectedNodeName = nil
        } else {
            var buf = [CChar](repeating: 0, count: 64)
            editor.nodeName(id, &buf, 64)
            selectedNodeID = id
            // Deviation from the brief's `String(cString: buf)`: that overload
            // (String from a value-type [CChar] array) is deprecated on this
            // toolchain. Going through the buffer pointer instead calls the
            // non-deprecated `String(cString: UnsafePointer<CChar>)` overload.
            selectedNodeName = buf.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
        }
        refreshOverlayState()
    }

    // MARK: - Radial-menu overlay mirrors

    /// Refreshes `radialAnchor`/`selectedNodeOp` from core. Called by
    /// `refreshSelectionMirrors()` above (selection changes) and directly
    /// after anything else that can move the anchor or change the op without
    /// changing the selection itself: camera gestures, drag/scale updates,
    /// viewport size changes, and op toggles.
    private func refreshOverlayState() {
        let anchor = editor.projectSelectedAnchor()
        radialAnchor = anchor.visible ? CGPoint(x: CGFloat(anchor.x), y: CGFloat(anchor.y)) : nil
        selectedNodeOp = selectedNodeID.map { editor.nodeOp($0) }
    }
}
