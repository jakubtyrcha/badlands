import SwiftUI
import ShapeshifterCore

// Deviation from the brief's stub (`enum EditorMode: CaseIterable`): adding
// Hashable is required both for `mode == .camera` guards below and for
// ForEach(EditorMode.allCases, id: \.self) in ModeBar.
enum EditorMode: CaseIterable, Hashable {
    case select, spawn, modify, camera
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

    /// Single entry point for every mode change (buttons, keys, future
    /// shortcuts) — keep it that way so later milestones have one place to
    /// hook mode-transition side effects (e.g. resetting spawn/gizmo state).
    func setMode(_ m: EditorMode) {
        mode = m
    }

    // MARK: - Raw input, called by the viewport.

    func handleMouseDown(_ p: CGPoint) {
        switch mode {
        case .select:
            let r = editor.pick(Float(p.x), Float(p.y))
            editor.select(r.node_id) // miss returns kInvalidNode -> clears
            refreshSelectionMirrors()
        case .spawn:
            let s = editor.spawn(spawnShape, spawnOp, Float(p.x), Float(p.y))
            guard s.node_id != sq.kInvalidNode else { return } // zero-size viewport guard in core
            // Selection was already made by core (Editor::spawn calls
            // select() internally); the mirrors + mode switch are the VM's
            // job, same division of labor as .select's pick+select above.
            refreshSelectionMirrors()
            setMode(.modify)
        case .modify, .camera:
            break // no-op until later milestones
        }
    }

    func handleMouseDragged(_ p: CGPoint, delta: CGSize) {
        // Drag-move / gizmo interaction land in later milestones.
    }

    func handleMouseUp(_ p: CGPoint) {
        // Selection is click-driven (handleMouseDown); drag/gizmo interaction
        // land in later milestones.
    }

    func handleScroll(dx: CGFloat, dy: CGFloat, shiftHeld: Bool) {
        guard mode == .camera else { return }
        if shiftHeld {
            editor.cameraPan(Float(dx), Float(dy))
        } else {
            editor.cameraOrbit(Float(dx), Float(dy))
        }
    }

    func handleMagnify(_ delta: CGFloat) {
        guard mode == .camera else { return }
        editor.cameraZoom(Float(delta))
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

    private func refreshSelectionMirrors() {
        let id = editor.selectedNode()
        // M4 deferred finding resolved: kInvalidNode moved to the interop
        // header (ShapeshifterCore.h) in this milestone specifically so
        // Swift could import it as a named sentinel instead of hardcoding -1.
        guard id != sq.kInvalidNode else {
            selectedNodeID = nil
            selectedNodeName = nil
            return
        }
        var buf = [CChar](repeating: 0, count: 64)
        editor.nodeName(id, &buf, 64)
        selectedNodeID = id
        // Deviation from the brief's `String(cString: buf)`: that overload
        // (String from a value-type [CChar] array) is deprecated on this
        // toolchain. Going through the buffer pointer instead calls the
        // non-deprecated `String(cString: UnsafePointer<CChar>)` overload.
        selectedNodeName = buf.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
    }
}
