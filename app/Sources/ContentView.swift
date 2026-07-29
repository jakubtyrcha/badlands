import SwiftUI
import ShapeshifterCore

struct ContentView: View {
    let editor = sq.Editor.create()!

    var body: some View {
        Text("shapeshifter — core ping: \(editor.ping())")
            .frame(minWidth: 800, minHeight: 600)
    }
}
