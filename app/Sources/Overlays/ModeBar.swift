import SwiftUI

/// Top-left bar of the 4 mode buttons. Buttons call `vm.setMode` — the
/// single entry point for mode changes — so this view never needs to know
/// about anything besides which mode is currently active.
struct ModeBar: View {
    let vm: EditorViewModel

    private struct Info {
        let symbol: String
        let name: String
        let key: String
    }

    private func info(for mode: EditorMode) -> Info {
        switch mode {
        case .select: return Info(symbol: "cursorarrow", name: "Select", key: "1")
        case .spawn: return Info(symbol: "plus.square", name: "Spawn", key: "2")
        case .modify: return Info(symbol: "arrow.up.and.down.and.arrow.left.and.right", name: "Modify", key: "3")
        case .camera: return Info(symbol: "rotate.3d", name: "Camera", key: "4")
        }
    }

    var body: some View {
        HStack(spacing: 4) {
            ForEach(EditorMode.allCases, id: \.self) { mode in
                let active = vm.mode == mode
                let info = info(for: mode)
                Button {
                    vm.setMode(mode)
                } label: {
                    Image(systemName: info.symbol)
                        .font(.system(size: 16, weight: .medium))
                        .frame(width: 28, height: 28)
                }
                .buttonStyle(.plain)
                .background(active ? Color.accentColor.opacity(0.35) : .clear, in: RoundedRectangle(cornerRadius: 6))
                .help("\(info.name) — \(info.key)")
            }
        }
        .padding(6)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
    }
}
