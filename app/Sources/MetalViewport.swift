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
    var onMouseDown: ((CGPoint) -> Void)?
    var onMouseDragged: ((CGPoint) -> Void)?
    var onMouseUp: ((CGPoint) -> Void)?
    /// (dx, dy, shiftHeld)
    var onScroll: ((CGFloat, CGFloat, Bool) -> Void)?
    var onMagnify: ((CGFloat) -> Void)?
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

    override func mouseDown(with event: NSEvent) {
        onMouseDown?(convert(event.locationInWindow, from: nil))
    }

    override func mouseDragged(with event: NSEvent) {
        onMouseDragged?(convert(event.locationInWindow, from: nil))
    }

    override func mouseUp(with event: NSEvent) {
        onMouseUp?(convert(event.locationInWindow, from: nil))
    }

    override func scrollWheel(with event: NSEvent) {
        onScroll?(event.scrollingDeltaX, event.scrollingDeltaY, event.modifierFlags.contains(.shift))
    }

    override func magnify(with event: NSEvent) {
        onMagnify?(event.magnification)
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
        autoreleasepool { // load-bearing: drains metal-cpp autoreleased objects each frame
            editor.render(Unmanaged.passUnretained(update.drawable as AnyObject).toOpaque())
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

        view.onMouseDown = { [vm] p in vm.handleMouseDown(p) }
        view.onMouseDragged = { [vm] p in vm.handleMouseDragged(p) }
        view.onMouseUp = { [vm] p in vm.handleMouseUp(p) }
        view.onScroll = { [vm] dx, dy, shiftHeld in vm.handleScroll(dx: dx, dy: dy, shiftHeld: shiftHeld) }
        view.onMagnify = { [vm] delta in vm.handleMagnify(delta) }
        view.onKeyDown = { [vm] characters in vm.handleKeyDown(characters) }

        return view
    }

    func updateNSView(_ nsView: ViewportNSView, context: Context) {
        // no-op
    }
}
