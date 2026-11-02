import SwiftUI

@main
struct OmniDemoApp: App {
    @StateObject private var store = DemoStore()
    var body: some Scene {
        WindowGroup { DemoView().environmentObject(store).preferredColorScheme(.light) }
    }
}
