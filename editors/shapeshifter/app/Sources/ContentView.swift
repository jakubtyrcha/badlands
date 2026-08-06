import SwiftUI

struct ContentView: View {
    @State private var vm = EditorViewModel()

    var body: some View {
        ZStack(alignment: .topLeading) {
            MetalViewport(vm: vm)
                .ignoresSafeArea()
            VStack(alignment: .leading, spacing: 8) {
                ModeBar(vm: vm)
                SpawnOptionsBar(vm: vm)
            }
            .padding(12)
        }
        .overlay(alignment: .topTrailing) {
            InfoPanel(vm: vm)
                .padding(12)
        }
        .overlay {
            // Radial menu tracks the selected node's projected screen
            // position; hidden while dragging so it doesn't fight the
            // move/scale gesture it's driving. `.position` only hit-tests
            // RadialMenu's own button shapes (no full-screen contentShape),
            // so the rest of this overlay stays click-through to the
            // viewport underneath.
            //
            // `radialAnchor` is computed by core against the viewport's own
            // (ignoresSafeArea) bounds, while this overlay positions it in
            // the enclosing ZStack's safe-area coordinate space. Those two
            // spaces coincide today only because this is a titled window
            // with zero safe-area insets; if window chrome ever changes that
            // (e.g. a toolbar), this anchor would need reconciling against
            // the viewport's actual frame instead of assuming they match.
            if vm.mode == .edit, let anchor = vm.radialAnchor, vm.selectedNodeID != nil, !vm.isDragging {
                RadialMenu(vm: vm)
                    .position(anchor)
            }
        }
        .frame(minWidth: 800, minHeight: 600)
    }
}
