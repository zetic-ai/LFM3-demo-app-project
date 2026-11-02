import AppKit
import Foundation

guard CommandLine.arguments.count == 3 else { fatalError("usage: render_icon.swift ZeticSymbol.svg OUTPUT.png") }
guard let symbol = NSImage(contentsOfFile: CommandLine.arguments[1]) else { fatalError("Cannot read official SVG symbol") }
guard let cgContext = CGContext(data: nil, width: 1024, height: 1024, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else { fatalError("Cannot allocate icon") }
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(cgContext: cgContext, flipped: false)
NSColor(calibratedRed: 52.0/255, green: 169.0/255, blue: 163.0/255, alpha: 1).setFill()
NSRect(x: 0, y: 0, width: 1024, height: 1024).fill()
// Preserve the official 35:34 aspect ratio, with a uniform brand-color canvas.
let height = 1024.0 * 34 / 35
symbol.draw(in: NSRect(x: 0, y: (1024-height)/2, width: 1024, height: height))
NSGraphicsContext.restoreGraphicsState()
guard let cgImage = cgContext.makeImage(), let png = NSBitmapImageRep(cgImage: cgImage).representation(using: .png, properties: [:]) else { fatalError("Cannot render icon") }
try png.write(to: URL(fileURLWithPath: CommandLine.arguments[2]))
