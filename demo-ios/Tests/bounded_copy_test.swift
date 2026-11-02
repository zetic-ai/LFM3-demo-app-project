import Foundation

@main
struct BoundedCopyTest {
    static func check(_ value: Bool, _ message: String) throws { if !value { throw NSError(domain: message, code: 1) } }
    static func main() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let payload = Data(repeating: 37, count: 2 * 1024 * 1024)
        let source = root.appendingPathComponent("source.wav")
        try payload.write(to: source)
        let folder = root.appendingPathComponent("copies")
        let copied = try BoundedMediaCopy.copy(source: source, directory: folder, limit: UInt64(payload.count))
        try check(try Data(contentsOf: copied) == payload, "bounded copy payload mismatch")
        let before = try Data(contentsOf: source)
        do { _ = try BoundedMediaCopy.copy(source: source, directory: folder, limit: 1024); throw NSError(domain: "known oversize accepted", code: 1) }
        catch BoundedMediaCopy.Failure.tooLarge {}
        let partial = root.appendingPathComponent("unknown-size.part")
        do { try BoundedMediaCopy.copy(stream: InputStream(data: payload), destination: partial, limit: 1024); throw NSError(domain: "unknown oversize accepted", code: 1) }
        catch BoundedMediaCopy.Failure.tooLarge {}
        try check(!FileManager.default.fileExists(atPath: partial.path), "partial output survived failure")
        do { try BoundedMediaCopy.copy(stream: InputStream(data: Data()), destination: source, limit: 10); throw NSError(domain: "existing path overwritten", code: 1) }
        catch BoundedMediaCopy.Failure.destinationExists {}
        try check(try Data(contentsOf: source) == before, "source changed")
        print("bounded known/unknown-size streaming, partial cleanup and no-overwrite PASS")
    }
}
