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
        .frame(minWidth: 800, minHeight: 600)
    }
}
