import Darwin
import Foundation

enum BoundedMediaCopy {
    enum Failure: LocalizedError {
        case tooLarge, cannotOpen, destinationExists, readFailed
        var errorDescription: String? {
            switch self {
            case .tooLarge: "The input exceeds the supported file-size limit (image 128 MiB, audio 256 MiB)."
            case .cannotOpen: "The input file cannot be opened."
            case .destinationExists: "The temporary output path is unavailable."
            case .readFailed: "The input could not be read completely."
            }
        }
    }
    static func copy(source: URL, directory: URL, limit: UInt64) throws -> URL {
        if let size = try? source.resourceValues(forKeys: [.fileSizeKey]).fileSize, size >= 0, UInt64(size) > limit { throw Failure.tooLarge }
        guard let stream = InputStream(url: source) else { throw Failure.cannotOpen }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let ext = source.pathExtension.count <= 16 ? source.pathExtension : "bin"
        let destination = directory.appendingPathComponent(UUID().uuidString).appendingPathExtension(ext)
        try copy(stream: stream, destination: destination, limit: limit)
        return destination
    }
    // This path also bounds providers that cannot report a file size in advance.
    static func copy(stream: InputStream, destination: URL, limit: UInt64) throws {
        let fd = open(destination.path, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0o600)
        guard fd >= 0 else { throw Failure.destinationExists }
        let handle = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
        var success = false
        defer { stream.close(); try? handle.close(); if !success { try? FileManager.default.removeItem(at: destination) } }
        stream.open()
        guard stream.streamError == nil else { throw Failure.cannotOpen }
        var buffer = [UInt8](repeating: 0, count: 1024 * 1024)
        var total: UInt64 = 0
        while true {
            let count = stream.read(&buffer, maxLength: buffer.count)
            guard count >= 0 else { throw Failure.readFailed }
            if count == 0 { break }
            guard UInt64(count) <= limit - total else { throw Failure.tooLarge }
            try handle.write(contentsOf: Data(buffer.prefix(count)))
            total += UInt64(count)
        }
        try handle.synchronize()
        success = true
    }
}
