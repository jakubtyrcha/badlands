import SwiftUI
import ShapeshifterCore

struct ContentView: View {
    let editor = sq.Editor.create()!

    var body: some View {
        MetalViewport(editor: editor)
            .ignoresSafeArea()
            .frame(minWidth: 800, minHeight: 600)
    }
}
