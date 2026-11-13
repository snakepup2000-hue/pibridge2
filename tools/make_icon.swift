import AppKit
import Foundation

// Draws the PiBridge icon: a dark rounded tile with one continuous signal
// trace — a smooth PCM sine on the left, a sparse DSD pulse train on the right.
// The pulse count is low on purpose: a dense bitstream turns to mush when
// iconutil downsamples to 16 px.
//
// usage: make_icon <out.png> <accent: blue|amber>

let args = CommandLine.arguments
let outPath = args.count > 1 ? args[1] : "icon.png"
let accentName = args.count > 2 ? args[2] : "blue"

let accent: NSColor = accentName == "amber"
    ? NSColor(srgbRed: 0.93, green: 0.66, blue: 0.28, alpha: 1)
    : NSColor(srgbRed: 0.35, green: 0.72, blue: 0.95, alpha: 1)

let side = 1024
let image = NSImage(size: NSSize(width: side, height: side))
image.lockFocus()
let ctx = NSGraphicsContext.current!.cgContext
ctx.setAllowsAntialiasing(true)

// Tile
let tile = NSRect(x: 92, y: 92, width: 840, height: 840)
let tilePath = NSBezierPath(roundedRect: tile, xRadius: 196, yRadius: 196)
NSGradient(colors: [
    NSColor(srgbRed: 0.085, green: 0.100, blue: 0.118, alpha: 1),
    NSColor(srgbRed: 0.028, green: 0.033, blue: 0.042, alpha: 1),
])!.draw(in: tilePath, angle: -90)
NSColor.white.withAlphaComponent(0.09).setStroke()
tilePath.lineWidth = 3
tilePath.stroke()

// One trace, left to right, at mid height.
let midY: CGFloat = 512
let amp: CGFloat = 122
let x0: CGFloat = 196
let x1: CGFloat = 828
let splitX: CGFloat = 512     // where the sine hands over to the bitstream

let trace = NSBezierPath()
trace.lineWidth = 50
trace.lineCapStyle = .round
trace.lineJoinStyle = .round

// Smooth half: 1.75 cycles of a sine.
func sineY(_ x: CGFloat) -> CGFloat {
    let span = splitX - x0
    let cycles: CGFloat = 1.75
    let phase = 2 * CGFloat.pi * cycles * (x - x0) / span
    return midY + amp * sin(phase)
}
trace.move(to: NSPoint(x: x0, y: sineY(x0)))
var x = x0
while x <= splitX {
    trace.line(to: NSPoint(x: x, y: sineY(x)))
    x += 4
}

// Digital half: four pulses, each a square swing the full amplitude.
let pulses = 4
let pulseSpan = (x1 - splitX) / CGFloat(pulses)
let shoulder: CGFloat = 16
var px = splitX
for i in 0..<pulses {
    let high = i % 2 == 0
    let y = midY + (high ? amp : -amp)
    trace.line(to: NSPoint(x: px + shoulder, y: y))
    trace.line(to: NSPoint(x: px + pulseSpan - shoulder, y: y))
    px += pulseSpan
}
trace.line(to: NSPoint(x: x1, y: midY))

accent.setStroke()
trace.stroke()

// A soft inner glow under the trace so it holds up against a light wallpaper.
ctx.saveGState()
ctx.setShadow(offset: .zero, blur: 26, color: accent.withAlphaComponent(0.55).cgColor)
accent.withAlphaComponent(0.5).setStroke()
trace.stroke()
ctx.restoreGState()

image.unlockFocus()

guard let tiff = image.tiffRepresentation,
      let rep = NSBitmapImageRep(data: tiff),
      let png = rep.representation(using: .png, properties: [:]) else {
    FileHandle.standardError.write(Data("could not render icon\n".utf8))
    exit(1)
}
try png.write(to: URL(fileURLWithPath: outPath))
print("wrote \(outPath)")
