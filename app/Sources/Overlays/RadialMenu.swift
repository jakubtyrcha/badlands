import SwiftUI
import ShapeshifterCore

/// Sims-style semicircle menu anchored to the selected node's projected
/// screen position (`vm.radialAnchor`). Two ~36pt circular buttons sit on
/// the upper semicircle of radius 64pt around the anchor, at 135°/45°
/// (measured from +x, y up in screen terms) — i.e. top-left / top-right.
///
/// It held four until both gizmos went live: Move and Scale were tool arming,
/// and there is no tool to arm now that a selected node shows both of its
/// manipulators at once. What is left is what no handle can express — the
/// node's CSG op, and deleting it.
///
/// `ContentView` centers this view on the anchor via `.position(anchor)`,
/// which places the anchor at the CENTER of this view's own layout frame.
/// The `.frame` below is therefore a full square (radius+buttonRadius on
/// every side, not just the upper half actually used) so that center lands
/// exactly on the anchor; the empty lower half has no background or
/// `contentShape`, so — per SwiftUI's default hit-testing — it stays
/// click-through to the viewport rather than intercepting clicks.
struct RadialMenu: View {
    let vm: EditorViewModel

    private static let radius: CGFloat = 64
    private static let buttonSize: CGFloat = 36
    private static let extent = radius + buttonSize / 2 // half of the enclosing square's side

    var body: some View {
        ZStack {
            button(angleDegrees: 135,
                   symbol: vm.selectedNodeOp == .Subtract ? "minus.circle.fill" : "plus.circle.fill",
                   help: "Additive / Subtract") {
                vm.radialToggleOp()
            }
            button(angleDegrees: 45, symbol: "xmark", help: "Delete") {
                vm.deleteSelected()
            }
        }
        .frame(width: Self.extent * 2, height: Self.extent * 2)
    }

    /// One radial-menu button, offset from the ZStack's center by `radius`
    /// along `angleDegrees` (standard math convention: 0° = +x, 90° = +y-up).
    /// Screen space has y increasing downward, so the vertical offset is
    /// negated to turn "up" in the angle convention into "up" on screen.
    ///
    /// No `active` state: it existed to show which of Move/Scale was armed, and
    /// both entries went away with tool arming itself. Neither remaining button
    /// is a mode — they act and are done.
    private func button(angleDegrees: Double, symbol: String, help: String,
                         action: @escaping () -> Void) -> some View {
        let radians = angleDegrees * .pi / 180
        let dx = Self.radius * cos(radians)
        let dy = -Self.radius * sin(radians)
        return Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 15, weight: .medium))
                .frame(width: Self.buttonSize, height: Self.buttonSize)
                .background(Circle().fill(.regularMaterial))
        }
        .buttonStyle(.plain)
        .help(help)
        .offset(x: dx, y: dy)
    }
}
