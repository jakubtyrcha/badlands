import SwiftUI
import ShapeshifterCore

/// Sims-style semicircle menu anchored to the selected node's projected screen
/// position (`vm.radialAnchor`). Circular ~36pt buttons sit on the upper
/// semicircle of radius 64pt around the anchor, in three fixed seats at
/// 150°/90°/30° (measured from +x, y up in screen terms).
///
/// It held four until both gizmos went live: Move and Scale were tool arming,
/// and there is no tool to arm now that a selected node shows both of its
/// manipulators at once. What is left is what no handle can express — the
/// node's CSG op, its shape parameter, and deleting it.
///
/// **The three seats are peers.** An earlier draft gave the shape dial its own
/// arc on the lower semicircle, which read as a separate control bolted beneath
/// the menu rather than as part of it. The dial is the same kind of thing as
/// the op toggle — something about the selected node that no gizmo handle can
/// reach — so it sits in the same ring, at the same radius, styled the same
/// way, and shows its current value the way the op button shows its current op.
/// The seats never move: a shape with no parameter simply leaves the middle one
/// empty, so the op and delete buttons are always in the same place.
///
/// What makes the dial a dial is what happens when you *hold* it — see
/// `knob(...)`.
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
    private static let space = "radialMenu"

    /// Fixed seats. 60° apart rather than the 90° the two-button menu used, so
    /// three 36pt buttons on a 64pt radius keep a comfortable gap.
    private static let opSeat: Double = 150
    private static let dialSeat: Double = 90
    private static let deleteSeat: Double = 30

    /// Everything a dial gesture needs, in one optional so it is all-or-nothing:
    /// there is no state to leave half-set if the gesture ends abnormally.
    private struct DialDrag {
        let startFraction: CGFloat  // where the value was when the press landed
        let startAngle: Double      // and where the cursor was, unwrapped
        var lastRawAngle: Double    // previous atan2 result, for unwrapping
        var unwrappedAngle: Double  // accumulated, so sweeping past ±180° is continuous
    }
    @State private var drag: DialDrag? = nil

    var body: some View {
        ZStack {
            button(seat: Self.opSeat,
                   symbol: vm.selectedNodeOp == .Subtract ? "minus.circle.fill" : "plus.circle.fill",
                   help: "Additive / Subtract") {
                vm.radialToggleOp()
            }
            button(seat: Self.deleteSeat, symbol: "xmark", help: "Delete") {
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
    /// along its seat angle (standard math convention: 0° = +x, 90° = +y-up).
    /// Screen space has y increasing downward, so the vertical offset is
    /// negated to turn "up" in the angle convention into "up" on screen.
    ///
    /// No `active` state: it existed to show which of Move/Scale was armed, and
    /// both entries went away with tool arming itself. Neither button is a
    /// mode — they act and are done.
    private func button(seat: Double, symbol: String, help: String,
                         action: @escaping () -> Void) -> some View {
        let offset = Self.offset(degrees: seat)
        return Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 15, weight: .medium))
                .frame(width: Self.buttonSize, height: Self.buttonSize)
                .background(Circle().fill(.regularMaterial))
        }
        .buttonStyle(.plain)
        .help(help)
        // Dimmed while the dial is turning, so the control being used is the
        // one that reads. Mirrors how hovering one gizmo dims the other.
        .opacity(drag == nil ? 1.0 : 0.35)
        .offset(x: offset.x, y: offset.y)
    }

    // MARK: - The shape dial

    @ViewBuilder
    private func dial(spec: sq.ShapeParamSpec, value: Float) -> some View {
        let fraction = Self.fraction(of: value, in: spec)
        ZStack {
            if let drag {
                // Drawn, never grabbed: the gesture already has capture by the
                // time any of this exists, and leaving it hit-testable would
                // claim a chunk of the menu that should reach the viewport
                // underneath.
                track(spec: spec, drag: drag, fraction: fraction)
                    .allowsHitTesting(false)
                readout(spec: spec, drag: drag, value: value, fraction: fraction)
                    .allowsHitTesting(false)
            }
            knob(spec: spec, value: value, fraction: fraction)
        }
    }

    /// At rest this is a button like the other two, in its own seat, showing its
    /// value — a ring gauge around the rim and the number in the middle, so the
    /// parameter is legible without touching anything.
    ///
    /// Held, it becomes the thing you turn. The arc is laid out so the CURRENT
    /// VALUE sits exactly under the press point, which is what makes the
    /// mapping absolute and the grab jump-free at the same time: at zero sweep
    /// the value is unchanged no matter where on the button you pressed. Angles
    /// accumulate unwrapped rather than being folded into a fixed span, so the
    /// cursor can circle the anchor freely and every value stays reachable from
    /// any starting point.
    private func knob(spec: sq.ShapeParamSpec, value: Float, fraction: CGFloat) -> some View {
        let angle = drag.map { Self.angle(ofFraction: fraction, in: $0) } ?? Self.dialSeat
        let offset = Self.offset(degrees: angle)
        let active = drag != nil
        return ZStack {
            Circle().fill(active ? AnyShapeStyle(Color.accentColor) : AnyShapeStyle(.regularMaterial))
            Circle()
                .trim(from: 0, to: fraction)
                .stroke(active ? Color.white.opacity(0.9) : Color.accentColor,
                        style: StrokeStyle(lineWidth: 3, lineCap: .round))
                .rotationEffect(.degrees(-90)) // start the gauge at 12 o'clock
                .padding(2)
            Text(Self.label(spec: spec, value: value))
                .font(.system(size: 11, weight: .semibold, design: .rounded))
                .foregroundStyle(active ? AnyShapeStyle(.white) : AnyShapeStyle(.primary))
        }
        .frame(width: Self.buttonSize, height: Self.buttonSize)
        .help(Self.help(for: vm.selectedNodeShape))
        .offset(x: offset.x, y: offset.y)
        .gesture(
            // minimumDistance 0 so the press itself opens the dial: it is a
            // hold-and-turn, and waiting for travel would leave the first few
            // degrees unexplained.
            DragGesture(minimumDistance: 0, coordinateSpace: .named(Self.space))
                .onChanged { gesture in
                    let raw = Self.rawDegrees(at: gesture.location)
                    if drag == nil {
                        // The turn is ONE undo step. Without this bracket every
                        // mouse-move would be its own entry, and sweeping the
                        // dial across its range would cost twenty of them.
                        vm.beginInteraction("Shape")
                        drag = DialDrag(startFraction: fraction, startAngle: raw,
                                        lastRawAngle: raw, unwrappedAngle: raw)
                    } else {
                        // Unwrap: atan2 jumps by 360° across the -x axis, and a
                        // raw difference there would read as most of the range
                        // travelled in one event.
                        var step = raw - drag!.lastRawAngle
                        if step > 180 { step -= 360 } else if step < -180 { step += 360 }
                        drag!.unwrappedAngle += step
                        drag!.lastRawAngle = raw
                    }
                    guard let drag else { return }
                    // Clockwise (rightward) increases, matching the gauge.
                    let swept = drag.startAngle - drag.unwrappedAngle
                    let target = drag.startFraction + CGFloat(swept / 180.0)
                    vm.setSelectedShapeParam(Self.value(atFraction: target, in: spec))
                }
                // Guarded on `drag`, exactly like the disappear path below, so
                // the pair is symmetric. If the knob vanished mid-gesture and
                // onDisappear already closed the interaction, an unconditional
                // end here would decrement an OUTER interaction's refcount --
                // committing it early and splitting one gesture into two undo
                // entries.
                .onEnded { _ in
                    guard drag != nil else { return }
                    drag = nil
                    vm.endInteraction()
                }
        )
        // The knob only exists while the selection has a parameter. If that
        // stops being true mid-gesture — a delete, a selection change — onEnded
        // never arrives, and a surviving `drag` would apply the previous
        // gesture's offset to the next press and jump the value on touch-down.
        //
        // The interaction has to be closed here for the same reason, and it
        // matters more: an interaction left open would swallow every later edit
        // into one undo entry that never closes.
        .onDisappear {
            if drag != nil {
                drag = nil
                vm.endInteraction()
            }
        }
    }

    private func track(spec: sq.ShapeParamSpec, drag: DialDrag, fraction: CGFloat) -> some View {
        ZStack {
            Self.arc(from: 0, to: 1, drag: drag)
                .stroke(Color.primary.opacity(0.18), style: StrokeStyle(lineWidth: 6, lineCap: .round))
            Self.arc(from: 0, to: fraction, drag: drag)
                .stroke(Color.accentColor, style: StrokeStyle(lineWidth: 6, lineCap: .round))
            // One dot per reachable value, so the snapping is visible as
            // detents rather than felt as stickiness. The step comes from the
            // shape's spec, which is also what core snaps against.
            ForEach(0..<Self.detentCount(spec), id: \.self) { i in
                let f = CGFloat(i) / CGFloat(max(Self.detentCount(spec) - 1, 1))
                Circle()
                    .fill(Color.primary.opacity(0.35))
                    .frame(width: 3, height: 3)
                    .offset(Self.offsetSize(degrees: Self.angle(ofFraction: f, in: drag)))
            }
        }
    }

    private func readout(spec: sq.ShapeParamSpec, drag: DialDrag,
                         value: Float, fraction: CGFloat) -> some View {
        // Just outside the knob on the same ray, so it never covers the track
        // it is describing.
        let offset = Self.offset(degrees: Self.angle(ofFraction: fraction, in: drag),
                                 radius: Self.radius + 28)
        return Text("\(Self.help(for: vm.selectedNodeShape))  \(Self.label(spec: spec, value: value))")
            .font(.system(size: 11, weight: .semibold, design: .rounded))
            .foregroundStyle(.white)
            .padding(.horizontal, 7)
            .padding(.vertical, 3)
            .background(Capsule().fill(Color.accentColor))
            .fixedSize()
            .offset(x: offset.x, y: offset.y)
    }

    // MARK: - Angle <-> value

    /// Where a value sits during a gesture. The press angle anchors the value
    /// the press started on, and a full range is 180° of sweep either side of
    /// it — so both ends are always within half a turn of wherever you grabbed.
    private static func angle(ofFraction fraction: CGFloat, in drag: DialDrag) -> Double {
        drag.startAngle - Double(fraction - drag.startFraction) * 180
    }

    private static func fraction(of value: Float, in spec: sq.ShapeParamSpec) -> CGFloat {
        let span = spec.max_value - spec.min_value
        guard span > 0 else { return 0 }
        return CGFloat(min(max((value - spec.min_value) / span, 0), 1))
    }

    private static func value(atFraction fraction: CGFloat, in spec: sq.ShapeParamSpec) -> Float {
        let clamped = min(max(fraction, 0), 1)
        return spec.min_value + Float(clamped) * (spec.max_value - spec.min_value)
    }

    /// Cursor angle about the menu's centre. Deliberately NOT folded into any
    /// span — the caller unwraps it instead. Folding was how an earlier version
    /// made part of the range unreachable when the knob was grabbed off-centre.
    private static func rawDegrees(at location: CGPoint) -> Double {
        atan2(-(location.y - extent), location.x - extent) * 180 / .pi
    }

    private static func detentCount(_ spec: sq.ShapeParamSpec) -> Int {
        guard spec.step > 0 else { return 0 }
        return Int(((spec.max_value - spec.min_value) / spec.step).rounded()) + 1
    }

    /// Compact enough for a 36pt button: whole numbers for a side count, and a
    /// leading-dot fraction otherwise.
    private static func label(spec: sq.ShapeParamSpec, value: Float) -> String {
        if spec.integral { return String(Int(value.rounded())) }
        if value >= 0.995 { return "1" }
        return String(format: ".%02d", Int((value * 100).rounded()))
    }

    /// What the dial adjusts, per shape. Roundness is one idea on four shapes,
    /// which is why several of these read the same.
    private static func help(for shape: sq.Shape?) -> String {
        switch shape {
        case .Cone, .Pyramid: return "Tip"
        case .Prism: return "Sides"
        case .Cube, .Capsule, .Octahedron, .Vesica: return "Roundness"
        default: return "Shape"
        }
    }

    // MARK: - Geometry

    private static func offset(degrees: Double, radius: CGFloat = radius) -> CGPoint {
        let radians = degrees * .pi / 180
        return CGPoint(x: radius * cos(radians), y: -radius * sin(radians))
    }

    private static func offsetSize(degrees: Double) -> CGSize {
        let point = offset(degrees: degrees)
        return CGSize(width: point.x, height: point.y)
    }

    /// Built from line segments rather than `Path.addArc`, whose sweep
    /// direction has to be reasoned about against SwiftUI's flipped y before it
    /// can be trusted. At this radius the polyline is indistinguishable, and it
    /// shares `angle(ofFraction:in:)` with the knob and the detents — so the
    /// drawn arc cannot drift away from the things riding it.
    private static func arc(from start: CGFloat, to end: CGFloat, drag: DialDrag) -> Path {
        var path = Path()
        let steps = 48
        for i in 0...steps {
            let f = start + (end - start) * CGFloat(i) / CGFloat(steps)
            let o = offset(degrees: angle(ofFraction: f, in: drag))
            let p = CGPoint(x: extent + o.x, y: extent + o.y)
            if i == 0 { path.move(to: p) } else { path.addLine(to: p) }
        }
        return path
    }
}
