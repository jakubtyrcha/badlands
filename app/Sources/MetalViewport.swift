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

/// Hosts the Metal viewport `NSView` in SwiftUI and wires up size/window
/// notifications to `core` and the display-link driver.
struct MetalViewport: NSViewRepresentable {
    let editor: sq.Editor

    final class Coordinator {
        var driver: DisplayLinkDriver?
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> ViewportNSView {
        let view = ViewportNSView()
        editor.attachLayer(Unmanaged.passUnretained(view.metalLayer).toOpaque())

        view.onSizeChange = { w, h, scale in
            editor.setViewportSize(Float(w), Float(h), Float(scale))
        }

        let driver = DisplayLinkDriver(metalLayer: view.metalLayer, editor: editor)
        context.coordinator.driver = driver

        view.onWindowChange = { inWindow in
            driver.isPaused = !inWindow
        }

        return view
    }

    func updateNSView(_ nsView: ViewportNSView, context: Context) {
        // no-op
    }
}
