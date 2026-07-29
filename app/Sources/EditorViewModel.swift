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

    /// Single entry point for every mode change (buttons, keys, future
    /// shortcuts) — keep it that way so later milestones have one place to
    /// hook mode-transition side effects (e.g. resetting spawn/gizmo state).
    func setMode(_ m: EditorMode) {
        mode = m
    }

    // MARK: - Raw input, called by the viewport.

    func handleMouseDown(_ p: CGPoint) {
        // Selection lands in M4.
    }

    func handleMouseDragged(_ p: CGPoint, delta: CGSize) {
        // Drag-move / gizmo interaction land in later milestones.
    }

    func handleMouseUp(_ p: CGPoint) {
        // Selection lands in M4.
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
}
