import Foundation
@main struct ScenarioTest {
    static func main() throws {
        let absent = try ScenarioLaunch.parse(["app"]); precondition(absent == nil)
        for name in ScenarioLaunch.names { let value = try ScenarioLaunch.parse(["app", "--scenario", name]); precondition(value == name) }
        for args in [["--scenario"], ["--scenario", "invalid"], ["--scenario", "tts", "--scenario", "asr"]] {
            do { _ = try ScenarioLaunch.parse(args); fatalError("invalid scenario accepted") } catch {}
        }
        print("opt-in scenario parsing: six scenarios, absent/default and invalid arguments PASS")
    }
}
