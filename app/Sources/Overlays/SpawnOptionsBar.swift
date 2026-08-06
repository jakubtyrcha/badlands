import SwiftUI
import ShapeshifterCore

/// Second row below the ModeBar, visible only in `.spawn` mode: shape and
/// operation (additive/subtract) pickers for the next spawn. Styled to match
/// ModeBar's compact segmented buttons.
///
/// Eight shapes at 28pt with 4pt gaps is ~252pt, which still sits comfortably
/// under the mode bar in the 800pt minimum window — so the row stays flat
/// rather than becoming a grid or a menu.
struct SpawnOptionsBar: View {
    let vm: EditorViewModel

    private struct ShapeInfo {
        let symbol: String
        let help: String
        let value: sq.Shape
    }

    private struct OpInfo {
        let symbol: String
        let help: String
        let value: sq.Op
    }

    /// Order matches `sq.Shape`'s own, so the row reads in the same order as
    /// every table in core. `oval` for the vesica is the weakest of the eight —
    /// SF Symbols has no lens/almond glyph — and is the one to revisit first.
    private let shapes: [ShapeInfo] = [
        ShapeInfo(symbol: "cube", help: "Cube", value: .Cube),
        ShapeInfo(symbol: "circle", help: "Sphere", value: .Sphere),
        ShapeInfo(symbol: "cone", help: "Cone", value: .Cone),
        ShapeInfo(symbol: "capsule", help: "Capsule", value: .Capsule),
        ShapeInfo(symbol: "diamond", help: "Octahedron", value: .Octahedron),
        ShapeInfo(symbol: "pyramid", help: "Pyramid", value: .Pyramid),
        ShapeInfo(symbol: "hexagon", help: "Prism", value: .Prism),
        ShapeInfo(symbol: "oval", help: "Vesica", value: .Vesica),
    ]

    private let ops: [OpInfo] = [
        OpInfo(symbol: "plus.circle", help: "Additive", value: .Add),
        OpInfo(symbol: "minus.circle", help: "Subtract", value: .Subtract),
    ]

    var body: some View {
        if vm.mode == .spawn {
            HStack(spacing: 10) {
                HStack(spacing: 4) {
                    ForEach(shapes, id: \.help) { shape in
                        optionButton(symbol: shape.symbol, help: shape.help, active: vm.spawnShape == shape.value) {
                            vm.spawnShape = shape.value
                        }
                    }
                }
                Divider().frame(height: 18)
                HStack(spacing: 4) {
                    ForEach(ops, id: \.help) { op in
                        optionButton(symbol: op.symbol, help: op.help, active: vm.spawnOp == op.value) {
                            vm.spawnOp = op.value
                        }
                    }
                }
            }
            .padding(6)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
        }
    }

    private func optionButton(symbol: String, help: String, active: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 16, weight: .medium))
                .frame(width: 28, height: 28)
        }
        .buttonStyle(.plain)
        .background(active ? Color.accentColor.opacity(0.35) : .clear, in: RoundedRectangle(cornerRadius: 6))
        .help(help)
    }
}
