import SwiftUI

/// Right-edge panel showing the selected node's name. Visible only while a
/// node is selected; per the product spec this is name-only, no other
/// fields (transform/op/etc. arrive with later milestones' tools).
struct InfoPanel: View {
    let vm: EditorViewModel

    var body: some View {
        if let name = vm.selectedNodeName {
            VStack(alignment: .leading, spacing: 4) {
                Text("Selection")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(name)
                    .font(.system(.body, design: .monospaced))
            }
            .padding(10)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
        }
    }
}
