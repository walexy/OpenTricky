// Draws the app icon (1024x1024 PNG): swiftc make_icon.swift -o make_icon && ./make_icon out.png logo.png
import AppKit

let S: CGFloat = 1024
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 1024, pixelsHigh: 1024, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                           isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let ctx = NSGraphicsContext.current!.cgContext

func rgb(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> NSColor { NSColor(deviceRed: r / 255, green: g / 255, blue: b / 255, alpha: a) }

// macOS icon shape: rounded square inside the standard margin
let tile = NSRect(x: 100, y: 100, width: 824, height: 824)
let shape = NSBezierPath(roundedRect: tile, xRadius: 185, yRadius: 185)
ctx.saveGState()
shape.addClip()

// sky: deep violet at the top to hot orange at the horizon
NSGradient(colors: [rgb(38, 24, 92), rgb(150, 38, 120), rgb(255, 96, 52), rgb(255, 186, 64)],
           atLocations: [0, 0.45, 0.78, 1], colorSpace: .deviceRGB)!.draw(in: tile, angle: 90)

// sun
rgb(255, 232, 150, 0.95).setFill()
NSBezierPath(ovalIn: NSRect(x: 560, y: 520, width: 190, height: 190)).fill()
rgb(255, 232, 150, 0.25).setFill()
NSBezierPath(ovalIn: NSRect(x: 520, y: 480, width: 270, height: 270)).fill()

// far range, near range (snow with a shaded side)
func peak(_ pts: [(CGFloat, CGFloat)], _ c: NSColor) {
    let p = NSBezierPath(); p.move(to: NSPoint(x: pts[0].0, y: pts[0].1))
    for q in pts.dropFirst() { p.line(to: NSPoint(x: q.0, y: q.1)) }
    p.close(); c.setFill(); p.fill()
}
peak([(80, 100), (80, 380), (260, 540), (360, 440), (520, 600), (700, 430), (830, 520), (950, 380), (950, 100)], rgb(92, 50, 130))
peak([(80, 100), (80, 300), (300, 470), (440, 350), (620, 520), (800, 340), (950, 430), (950, 100)], rgb(236, 240, 255))
peak([(620, 520), (800, 340), (950, 430), (950, 100), (700, 100), (760, 330)], rgb(176, 188, 226))
peak([(300, 470), (440, 350), (520, 100), (330, 100), (380, 300)], rgb(176, 188, 226))

// the SSX Tricky logo (logo.png, from SteamGridDB logo 55991) across the middle, with a soft drop shadow
let logo = NSImage(contentsOfFile: CommandLine.arguments[2])!
let lw: CGFloat = 780, lh = lw * logo.size.height / logo.size.width
let shadow = NSShadow(); shadow.shadowColor = rgb(20, 10, 50, 0.55); shadow.shadowOffset = NSSize(width: 0, height: -10); shadow.shadowBlurRadius = 24
shadow.set()
logo.draw(in: NSRect(x: (S - lw) / 2, y: (S - lh) / 2 - 10, width: lw, height: lh), from: .zero, operation: .sourceOver, fraction: 1)
ctx.restoreGState()

// a thin inner edge for depth
rgb(255, 255, 255, 0.22).setStroke(); shape.lineWidth = 4; shape.stroke()
NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
