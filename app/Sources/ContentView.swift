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
            if vm.mode == .modify, let anchor = vm.radialAnchor, vm.selectedNodeID != nil, !vm.isDragging {
                RadialMenu(vm: vm)
                    .position(anchor)
            }
        }
        .frame(minWidth: 800, minHeight: 600)
    }
}
