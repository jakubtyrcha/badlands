import SwiftUI

@main
struct ShapeshifterApp: App {
    /// Hoisted out of `ContentView` so `.commands` can reach it: a menu is built
    /// beside the window, not inside its content, and the Edit menu's titles
    /// come from core's undo stack.
    @State private var vm = EditorViewModel()

    var body: some Scene {
        WindowGroup {
            ContentView(vm: vm)
        }
        .commands {
            // Replaces the standard Undo/Redo pair rather than adding a second
            // one. Core owns the ONE undo stack; there is deliberately no
            // NSUndoManager, because two stacks over one document is a bug
            // factory.
            //
            // ⇧⌘Z is the Mac redo. ⌘Y is accepted in the viewport's key handler
            // as a Windows-habit alias, but never shown here.
            CommandGroup(replacing: .undoRedo) {
                Button(vm.undoTitle) { vm.undo() }
                    .keyboardShortcut("z", modifiers: .command)
                    .disabled(!vm.canUndo)
                Button(vm.redoTitle) { vm.redo() }
                    .keyboardShortcut("z", modifiers: [.command, .shift])
                    .disabled(!vm.canRedo)
            }
        }
    }
}
