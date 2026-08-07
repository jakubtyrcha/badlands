import SwiftUI
import AppKit
import QuartzCore
import ShapeshifterCore

/// Backing `NSView` for the Metal viewport. Owns a `CAMetalLayer` and
/// reports size/backing-scale changes so `core` can size the drawable.
final class ViewportNSView: NSView {
    override var isFlipped: Bool { true } // mouse coords: top-left origin (project-wide convention)
    override var acceptsFirstResponder: Bool { true }

    override func makeBackingLayer() -> CALayer { CAMetalLayer() }

    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    /// (widthPts, heightPts, backingScale)
    var onSizeChange: ((CGFloat, CGFloat, CGFloat) -> Void)?
    var onWindowChange: ((Bool) -> Void)?

    // Raw-input forwarding, wired up by MetalViewport to the EditorViewModel.
    /// (point, modifierFlags) — modifiers ride along because the camera verb is
    /// chosen from them at mouse-down and held for the whole gesture.
    var onMouseDown: ((CGPoint, NSEvent.ModifierFlags) -> Void)?
    var onMouseDragged: ((CGPoint) -> Void)?
    var onMouseUp: ((CGPoint) -> Void)?
    var onMouseMoved: ((CGPoint) -> Void)?
    var onMouseExited: (() -> Void)?
    /// (dx, dy, location, phase, momentumPhase). The phases are what let scroll
    /// and pinch be real begin/update/end gestures on a trackpad; a legacy
    /// wheel reports none, and the view model treats that as a one-shot.
    var onScroll: ((CGFloat, CGFloat, CGPoint, NSEvent.Phase, NSEvent.Phase) -> Void)?
    /// (magnification, location, phase)
    var onMagnify: ((CGFloat, CGPoint, NSEvent.Phase) -> Void)?
    /// Returns true if the key was consumed.
    var onKeyDown: ((String) -> Bool)?

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        wantsLayer = true
    }

    override func layout() {
        super.layout()
        updateLayerProperties()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        updateLayerProperties()
    }

    private func updateLayerProperties() {
        let scale = window?.backingScaleFactor ?? 2.0
        metalLayer.contentsScale = scale
        metalLayer.wantsExtendedDynamicRangeContent = true
        onSizeChange?(bounds.width, bounds.height, scale)
    }

    override func viewWillMove(toWindow newWindow: NSWindow?) {
        super.viewWillMove(toWindow: newWindow)
        onWindowChange?(newWindow != nil)
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        window?.makeFirstResponder(self) // so keys work immediately, no click needed
    }

    // Move-gizmo hover needs un-buttoned mouse positions, which AppKit only
    // delivers to views with a tracking area (.inVisibleRect keeps the rect
    // in sync with the view automatically; rect: .zero is then ignored).
    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(
            rect: .zero,
            options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect],
            owner: self))
    }

    override func mouseMoved(with event: NSEvent) {
        onMouseMoved?(convert(event.locationInWindow, from: nil))
    }

    override func mouseExited(with event: NSEvent) {
        onMouseExited?()
    }

    override func mouseDown(with event: NSEvent) {
        onMouseDown?(convert(event.locationInWindow, from: nil), event.modifierFlags)
    }

    override func mouseDragged(with event: NSEvent) {
        onMouseDragged?(convert(event.locationInWindow, from: nil))
    }

    override func mouseUp(with event: NSEvent) {
        onMouseUp?(convert(event.locationInWindow, from: nil))
    }

    override func scrollWheel(with event: NSEvent) {
        onScroll?(event.scrollingDeltaX, event.scrollingDeltaY,
                  convert(event.locationInWindow, from: nil), event.phase, event.momentumPhase)
    }

    override func magnify(with event: NSEvent) {
        onMagnify?(event.magnification, convert(event.locationInWindow, from: nil), event.phase)
    }

    override func keyDown(with event: NSEvent) {
        let consumed = onKeyDown?(event.charactersIgnoringModifiers ?? "") ?? false
        if !consumed {
            super.keyDown(with: event)
        }
    }
}

/// Drives per-frame rendering off a `CAMetalDisplayLink`, forwarding the
/// presented drawable to core each callback.
@MainActor
final class DisplayLinkDriver: NSObject, CAMetalDisplayLinkDelegate {
    private let link: CAMetalDisplayLink
    private let editor: sq.Editor

    init(metalLayer: CAMetalLayer, editor: sq.Editor) {
        self.link = CAMetalDisplayLink(metalLayer: metalLayer)
        self.editor = editor
        super.init()
        link.delegate = self
        link.add(to: .main, forMode: .common)
    }

    var isPaused: Bool {
        get { link.isPaused }
        set { link.isPaused = newValue }
    }

    func invalidate() {
        link.invalidate()
    }

    func metalDisplayLink(_ link: CAMetalDisplayLink, needsUpdate update: CAMetalDisplayLink.Update) {
        // `update.drawable` is deliberately unused: the RHI's swapchain calls
        // nextDrawable itself, so this callback is only a tick. The
        // autoreleasepool stays load-bearing -- the RHI's Metal backend is ARC
        // Objective-C++ and drains here each frame.
        autoreleasepool {
            editor.render()
        }
    }
}

/// Hosts the Metal viewport `NSView` in SwiftUI, wires up size/window
/// notifications to `core` and the display-link driver, and routes raw
/// input events to the `EditorViewModel`.
struct MetalViewport: NSViewRepresentable {
    let vm: EditorViewModel

    final class Coordinator {
        var driver: DisplayLinkDriver?
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> ViewportNSView {
        let view = ViewportNSView()
        let editor = vm.editor
        editor.attachLayer(Unmanaged.passUnretained(view.metalLayer).toOpaque())

        view.onSizeChange = { [vm] w, h, scale in
            editor.setViewportSize(Float(w), Float(h), Float(scale))
            vm.handleViewportSizeChange()
        }

        let driver = DisplayLinkDriver(metalLayer: view.metalLayer, editor: editor)
        context.coordinator.driver = driver

        view.onWindowChange = { inWindow in
            driver.isPaused = !inWindow
        }

        view.onMouseDown = { [vm] p, modifiers in vm.handleMouseDown(p, modifiers: modifiers) }
        view.onMouseDragged = { [vm] p in vm.handleMouseDragged(p) }
        view.onMouseUp = { [vm] p in vm.handleMouseUp(p) }
        view.onMouseMoved = { [vm] p in vm.handleMouseMoved(p) }
        view.onMouseExited = { [vm] in vm.handleMouseExited() }
        view.onScroll = { [vm] dx, dy, point, phase, momentumPhase in
            vm.handleScroll(dx: dx, dy: dy, at: point, phase: phase, momentumPhase: momentumPhase)
        }
        view.onMagnify = { [vm] magnification, point, phase in
            vm.handleMagnify(magnification, at: point, phase: phase)
        }
        view.onKeyDown = { [vm] characters in vm.handleKeyDown(characters) }

        return view
    }

    func updateNSView(_ nsView: ViewportNSView, context: Context) {
        // no-op
    }
}
