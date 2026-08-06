import SwiftUI
import ShapeshifterCore

/// Sims-style menu anchored to the selected node's projected screen position
/// (`vm.radialAnchor`). Two ~36pt circular buttons sit on the UPPER semicircle
/// of radius 64pt around the anchor, at 135°/45° (measured from +x, y up in
/// screen terms) — i.e. top-left / top-right.
///
/// It held four until both gizmos went live: Move and Scale were tool arming,
/// and there is no tool to arm now that a selected node shows both of its
/// manipulators at once. What is left is what no handle can express — the
/// node's CSG op, and deleting it.
///
/// **Upper half acts, lower half sets.** The LOWER semicircle carries the shape
/// dial: a knob riding the same 64pt radius, whose resting angle *is* the
/// selected shape's profile parameter. That placement is what makes absolute
/// mapping and "no jump on grab" the same thing rather than opposites — you
/// always grab the knob where it already is, and one 180° sweep still reaches
/// both ends. Putting it on the upper semicircle instead would have meant
/// pressing a fixed button at 90° with the knob elsewhere, which is exactly the
/// jump-on-grab `gizmo.h` rules out for the scale handles.
///
/// Shapes whose proportions are fully described by their box (cube, sphere,
/// octahedron, vesica) have no parameter, so the lower half stays empty and
/// click-through for them, exactly as it was before the dial existed.
///
/// `ContentView` centers this view on the anchor via `.position(anchor)`,
/// which places the anchor at the CENTER of this view's own layout frame.
/// The `.frame` below is therefore a full square, so that center lands exactly
/// on the anchor; everything outside the buttons and the knob has no background
/// and no `contentShape`, so — per SwiftUI's default hit-testing — it stays
/// click-through to the viewport rather than intercepting clicks.
struct RadialMenu: View {
    let vm: EditorViewModel

    private static let radius: CGFloat = 64
    private static let buttonSize: CGFloat = 36
    private static let knobSize: CGFloat = 30
    private static let extent = radius + buttonSize / 2 // half of the enclosing square's side
    private static let space = "radialMenu"

    /// True only while the dial is held. The track, its detents and the numeric
    /// readout draw during that and no other time — the same bargain the
    /// placement grid strikes, where the thing that answers a mid-drag question
    /// is not on screen when nobody is asking it.
    @State private var dialActive = false
    /// Angle between the cursor and the knob at the moment of the press, held
    /// for the whole gesture. Without it, grabbing the knob anywhere but dead
    /// centre would snap the value by up to a detent before the drag even
    /// starts; with it the grab is exactly neutral. Captured at press and never
    /// re-read, the same idiom every drag in core uses.
    @State private var grabOffsetDegrees: Double? = nil

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
            if let spec = vm.selectedNodeParamSpec {
                dial(spec: spec, value: vm.selectedNodeParam ?? spec.default_value)
            }
        }
        .frame(width: Self.extent * 2, height: Self.extent * 2)
        .coordinateSpace(.named(Self.space))
    }

    // MARK: - The two action buttons

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
        let offset = Self.offset(angleDegrees: angleDegrees)
        return Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 15, weight: .medium))
                .frame(width: Self.buttonSize, height: Self.buttonSize)
                .background(Circle().fill(.regularMaterial))
        }
        .buttonStyle(.plain)
        .help(help)
        // Dimmed while the dial is turning, so the half of the menu being used
        // is the half that reads. Mirrors how hovering one gizmo dims the other.
        .opacity(dialActive ? 0.35 : 1.0)
        .offset(x: offset.x, y: offset.y)
    }

    // MARK: - The shape dial

    @ViewBuilder
    private func dial(spec: sq.ShapeParamSpec, value: Float) -> some View {
        let fraction = Self.fraction(of: value, in: spec)
        ZStack {
            if dialActive {
                // Drawn, never grabbed: the gesture already has capture by the
                // time any of this exists, and leaving it hit-testable would
                // claim a chunk of the lower half that should reach the
                // viewport underneath.
                track(spec: spec, fraction: fraction)
                    .allowsHitTesting(false)
                readout(spec: spec, value: value, fraction: fraction)
                    .allowsHitTesting(false)
            }
            knob(spec: spec, fraction: fraction)
        }
    }

    private func knob(spec: sq.ShapeParamSpec, fraction: CGFloat) -> some View {
        let offset = Self.offset(angleDegrees: Self.angleDegrees(fraction: fraction))
        return Circle()
            .fill(dialActive ? AnyShapeStyle(Color.accentColor) : AnyShapeStyle(.regularMaterial))
            .overlay(Circle().strokeBorder(Color.accentColor, lineWidth: dialActive ? 0 : 1.5))
            .frame(width: Self.knobSize, height: Self.knobSize)
            .offset(x: offset.x, y: offset.y)
            .help(spec.integral ? "Sides" : "Shape")
            .gesture(
                // minimumDistance 0 so the press itself opens the track: the
                // dial is a hold-and-turn, and waiting for travel would leave
                // the first few degrees unexplained.
                DragGesture(minimumDistance: 0, coordinateSpace: .named(Self.space))
                    .onChanged { gesture in
                        let cursor = Self.cursorDegrees(at: gesture.location)
                        if grabOffsetDegrees == nil {
                            dialActive = true
                            grabOffsetDegrees = cursor - Self.angleDegrees(fraction: fraction)
                        }
                        let target = cursor - (grabOffsetDegrees ?? 0)
                        vm.setSelectedShapeParam(Self.value(atAngle: target, in: spec))
                    }
                    .onEnded { _ in
                        dialActive = false
                        grabOffsetDegrees = nil
                    }
            )
    }

    private func track(spec: sq.ShapeParamSpec, fraction: CGFloat) -> some View {
        ZStack {
            Self.arc(from: 0, to: 1)
                .stroke(Color.primary.opacity(0.18), style: StrokeStyle(lineWidth: 6, lineCap: .round))
            Self.arc(from: 0, to: fraction)
                .stroke(Color.accentColor, style: StrokeStyle(lineWidth: 6, lineCap: .round))
            // One dot per reachable value, so the snapping is visible as
            // detents rather than felt as stickiness. The step comes from the
            // shape's spec, which is also what core snaps against.
            ForEach(0..<Self.detentCount(spec), id: \.self) { i in
                let f = CGFloat(i) / CGFloat(max(Self.detentCount(spec) - 1, 1))
                Circle()
                    .fill(Color.primary.opacity(0.35))
                    .frame(width: 3, height: 3)
                    .offset(Self.offsetSize(angleDegrees: Self.angleDegrees(fraction: f)))
            }
        }
    }

    private func readout(spec: sq.ShapeParamSpec, value: Float, fraction: CGFloat) -> some View {
        // Sits just outside the knob, on the same ray, so it never covers the
        // track it is describing.
        let offset = Self.offset(angleDegrees: Self.angleDegrees(fraction: fraction),
                                 radius: Self.radius + 26)
        return Text(spec.integral ? String(Int(value.rounded())) : String(format: "%.2f", value))
            .font(.system(size: 12, weight: .semibold, design: .rounded))
            .foregroundStyle(.white)
            .padding(.horizontal, 7)
            .padding(.vertical, 3)
            .background(Capsule().fill(Color.accentColor))
            .offset(x: offset.x, y: offset.y)
    }

    // MARK: - Angle <-> value

    /// The dial sweeps the LOWER semicircle: 180° (straight left) is the
    /// minimum, 270° (straight down) the middle, 360° (straight right) the
    /// maximum. Angles are the same math convention the two buttons use, so
    /// nothing on this menu measures angles two different ways.
    private static func angleDegrees(fraction: CGFloat) -> Double {
        180 + 180 * Double(fraction)
    }

    private static func fraction(of value: Float, in spec: sq.ShapeParamSpec) -> CGFloat {
        let span = spec.max_value - spec.min_value
        guard span > 0 else { return 0 }
        return CGFloat(min(max((value - spec.min_value) / span, 0), 1))
    }

    private static func value(atAngle degrees: Double, in spec: sq.ShapeParamSpec) -> Float {
        let fraction = (degrees - 180) / 180
        let clamped = min(max(fraction, 0), 1)
        return spec.min_value + Float(clamped) * (spec.max_value - spec.min_value)
    }

    /// Cursor angle about the menu's centre, folded onto the dial's [180, 360]
    /// sweep. Dragging up into the acted-on half holds whichever end is nearer
    /// rather than jumping to the other one, which is what keeps an overshoot
    /// past either end from reading as a wrap-around.
    private static func cursorDegrees(at location: CGPoint) -> Double {
        let dx = location.x - extent
        let dy = location.y - extent
        var degrees = atan2(-dy, dx) * 180 / .pi   // (-180, 180], y-up convention
        if degrees > 0 {
            degrees = degrees > 90 ? 180 : 360
        } else {
            degrees += 360
        }
        return degrees
    }

    private static func detentCount(_ spec: sq.ShapeParamSpec) -> Int {
        guard spec.step > 0 else { return 0 }
        return Int(((spec.max_value - spec.min_value) / spec.step).rounded()) + 1
    }

    // MARK: - Geometry

    private static func offset(angleDegrees: Double, radius: CGFloat = radius) -> CGPoint {
        let radians = angleDegrees * .pi / 180
        return CGPoint(x: radius * cos(radians), y: -radius * sin(radians))
    }

    private static func offsetSize(angleDegrees: Double) -> CGSize {
        let point = offset(angleDegrees: angleDegrees)
        return CGSize(width: point.x, height: point.y)
    }

    private static func point(fraction: CGFloat) -> CGPoint {
        let offset = offset(angleDegrees: angleDegrees(fraction: fraction))
        return CGPoint(x: extent + offset.x, y: extent + offset.y)
    }

    /// Built from line segments rather than `Path.addArc`, whose sweep
    /// direction has to be reasoned about against SwiftUI's flipped y before it
    /// can be trusted. At this radius the polyline is indistinguishable, and it
    /// shares `point(fraction:)` with the knob and the detents — so the drawn
    /// arc cannot drift away from the thing riding it.
    private static func arc(from start: CGFloat, to end: CGFloat) -> Path {
        var path = Path()
        let steps = 48
        for i in 0...steps {
            let f = start + (end - start) * CGFloat(i) / CGFloat(steps)
            let p = point(fraction: f)
            if i == 0 { path.move(to: p) } else { path.addLine(to: p) }
        }
        return path
    }
}
