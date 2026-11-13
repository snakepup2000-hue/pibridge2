import SwiftUI
import AppKit
import UniformTypeIdentifiers

@main
struct PiBridgeApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var engine = Engine.shared

    var body: some Scene {
        // Window, not WindowGroup: a WindowGroup treats every open-file event
        // (and every "New Window") as a new scene, which multiplied the UI.
        Window("PiBridge", id: "main") {
            ContentView()
                .environmentObject(engine)
                .frame(minWidth: 760, minHeight: 560)
                .onAppear { WindowPlacer.placeIfOffscreen() }
        }
        // contentMinSize, not contentSize: the instrument is worth making
        // bigger, and a restored frame from an older layout must not be able
        // to squeeze it below its minimum.
        .windowResizability(.contentMinSize)
        .defaultSize(width: 1060, height: 700)
        .commands {
            CommandGroup(replacing: .newItem) {}
        }
    }
}

/// SwiftUI persists window frames by name. A frame saved while a second
/// display was attached will reopen on that display, which is where it cannot
/// be seen. This pulls the main window back onto a screen that exists.
enum WindowPlacer {
    /// Both displays here are real, so a saved frame can legitimately land on
    /// the second monitor and look "missing" to someone at the main display.
    /// First run after this change centres on the main screen once; after that
    /// whatever position you choose is respected.
    static func placeIfOffscreen() {
        let defaults = UserDefaults.standard
        guard !defaults.bool(forKey: "didPlaceWindow") else { return }
        // SwiftUI restores the saved frame a moment after the view appears, and
        // the exact moment moves between releases. Poll briefly and stop as
        // soon as the window is on the main display.
        var attempts = 0
        func attempt() {
            attempts += 1
            guard let main = NSScreen.main,
                  let window = NSApp.windows.first(where: {
                      $0.isVisible && !$0.className.contains("Panel")
                  }) else {
                if attempts < 20 { DispatchQueue.main.asyncAfter(deadline: .now() + 0.1, execute: attempt) }
                return
            }
            let v = main.visibleFrame
            let onMain = v.intersects(window.frame)
            if onMain || attempts > 20 {
                if onMain { defaults.set(true, forKey: "didPlaceWindow") }
                return
            }
            let size = window.frame.size
            window.setFrameOrigin(NSPoint(x: v.midX - size.width / 2,
                                          y: v.midY - size.height / 2))
            if attempts < 20 { DispatchQueue.main.asyncAfter(deadline: .now() + 0.1, execute: attempt) }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1, execute: attempt)
    }

    /// Force back to the main display; used by the Settings button.
    static func recenterOnMain() {
        guard let main = NSScreen.main,
              let window = NSApp.windows.first(where: {
                  $0.isVisible && !$0.className.contains("Panel")
              }) else { return }
        let size = window.frame.size
        let v = main.visibleFrame
        window.setFrameOrigin(NSPoint(x: v.midX - size.width / 2,
                                      y: v.midY - size.height / 2))
        window.makeKeyAndOrderFront(nil)
    }
}

/// Receives file-open events so `open -a PiBridge track.flac` and Finder's
/// "Open With" both start playback immediately.
final class AppDelegate: NSObject, NSApplicationDelegate {
    func application(_ application: NSApplication, open urls: [URL]) {
        guard let url = urls.first else { return }
        Task { @MainActor in Engine.shared.handleOpen(url: url) }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    func applicationWillTerminate(_ notification: Notification) {
        // Stops the child engine and the Pi's aplay before we go away.
        MainActor.assumeIsolated { Engine.shared.shutdown() }
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // A saved window frame can point at a display that is no longer
        // attached (or at a coordinate beyond every screen), which makes the
        // app look dead: it launched, but the window is nowhere visible.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) {
            guard let window = NSApp.windows.first(where: { $0.isVisible && $0.contentView != nil })
            else { return }
            // NSScreen.main is the screen with the key window, NOT the primary
            // display, so ask for screens[0] (the one with the menu bar) and set
            // the frame explicitly rather than relying on center().
            let primary = NSScreen.screens.first ?? NSScreen.main
            if let area = primary?.visibleFrame {
                let mid = NSPoint(x: window.frame.midX, y: window.frame.midY)
                if !area.contains(mid) {
                    let size = window.frame.size
                    window.setFrame(NSRect(x: area.midX - size.width / 2,
                                           y: area.midY - size.height / 2,
                                           width: size.width, height: size.height),
                                    display: true)
                }
            }
            window.makeKeyAndOrderFront(nil)
            NSApp.activate(ignoringOtherApps: true)
        }
    }
}

private enum Ink {
    static let bg = Color(red: 0.055, green: 0.06, blue: 0.075)
    static let panel = Color(red: 0.095, green: 0.102, blue: 0.125)
    static let panelEdge = Color.white.opacity(0.07)
    static let text = Color(white: 0.92)
    static let dim = Color(white: 0.55)
    static let faint = Color(white: 0.36)
    static let accent = Color(red: 0.42, green: 0.82, blue: 0.72)
    static let warm = Color(red: 0.95, green: 0.72, blue: 0.36)
    static let bad = Color(red: 0.93, green: 0.42, blue: 0.42)
    static let rail = Color.white.opacity(0.13)
}

/// Health of one hop in the chain.
enum HopState { case idle, good, warn, bad }

struct ContentView: View {
    @EnvironmentObject var engine: Engine
    @State private var file: URL?
    @State private var showingSettings = false
    @State private var showingDiag = false
    @State private var diagText = ""
    @State private var loadingDiag = false
    @State private var dropTargeted = false
    @State private var hovered: Int?
    @State private var showingQueue = false

    var body: some View {
        VStack(spacing: 0) {
            signalPath
            Divider().overlay(Ink.panelEdge)
            HStack(spacing: 0) {
                depthPanel
                Divider().overlay(Ink.panelEdge)
                sidePanel
            }
            Divider().overlay(Ink.panelEdge)
            transport
        }
        .background(Ink.bg)
        .foregroundStyle(Ink.text)
        .sheet(isPresented: $showingSettings) { SettingsSheet().environmentObject(engine) }
        .sheet(isPresented: $showingDiag) { diagSheet }
        .onDrop(of: [.fileURL], isTargeted: $dropTargeted) { providers in
            guard let provider = providers.first else { return false }
            _ = provider.loadObject(ofClass: URL.self) { url, _ in
                guard let url else { return }
                Task { @MainActor in
                    file = url
                    engine.open(url: url)
                }
            }
            return true
        }
        .overlay(dropHighlight)
    }

    // MARK: Signal path

    /// The spine of the app: the actual chain, left to right, with the state
    /// each hop is really in. This replaces four unrelated dashboard panels.
    private var signalPath: some View {
        HStack(spacing: 0) {
            titleBlock
            Spacer(minLength: 12)
            HopNode(label: "ENCODE",
                    value: engine.status.formatLabel.isEmpty ? "—" : engine.status.formatLabel,
                    sub: engine.status.rateLabel,
                    state: engine.isRunning ? .good : .idle)
            HopLink(active: engine.isRunning)
            HopNode(label: "SSH",
                    value: engine.isRunning ? "linked" : "idle",
                    sub: engine.user.isEmpty ? "" : "\(engine.user)@\(engine.host)",
                    state: engine.isRunning ? .good : .idle)
            HopLink(active: engine.isRunning)
            HopNode(label: "RING",
                    value: engine.status.ringMs > 0
                        ? String(format: "%.1f ms", engine.status.ringMs) : "—",
                    sub: "jitter budget",
                    state: engine.status.ringMs > 0 ? .good : .idle)
            HopLink(active: engine.isRunning)
            HopNode(label: "DAC · DMA",
                    value: engine.status.devMeanMs > 0
                        ? String(format: "%.2f ms", engine.status.devMeanMs) : "—",
                    sub: engine.status.bufferMs > 0
                        ? String(format: "of %.0f ms buffer", engine.status.bufferMs) : "no lock",
                    state: dacState)
            Spacer(minLength: 8)
            headerButtons
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
        .background(Ink.panel)
    }

    private var dacState: HopState {
        guard engine.isRunning else { return .idle }
        let bad = engine.status.xruns + engine.status.underruns + engine.status.devXruns
        if bad > 0 { return .bad }
        // Margin is what matters: a fill that ever touches the floor is one
        // scheduling hiccup from an audible click.
        if engine.status.devMinMs > 0, engine.status.devMinMs <= 1.0 { return .warn }
        return .good
    }

    private var titleBlock: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 8) {
                Text("PiBridge 2")
                    .font(.system(size: 15, weight: .semibold, design: .rounded))
                phaseBadge
            }
            Text(engine.receiver == "aplay" ? "via aplay" : "via hqdac · fixed buffer")
                .font(.system(size: 9, design: .monospaced))
                .foregroundStyle(Ink.faint)
        }
    }

    private var phaseBadge: some View {
        let (label, color): (String, Color) = {
            switch engine.phase {
            case .idle: return ("IDLE", Ink.faint)
            case .connecting: return ("LINKING", Ink.warm)
            case .playing: return ("PLAYING", Ink.accent)
            case .stopping: return ("STOPPING", Ink.warm)
            case .failed: return ("ERROR", Ink.bad)
            }
        }()
        return Text(label)
            .font(.system(size: 8, weight: .bold, design: .monospaced))
            .tracking(0.8)
            .foregroundStyle(color)
            .padding(.horizontal, 6).padding(.vertical, 2)
            .background(Capsule().fill(color.opacity(0.14)))
    }

    private var headerButtons: some View {
        HStack(spacing: 10) {
            Button {
                loadingDiag = true
                showingDiag = true
                Task {
                    let t = await engine.runDiagnostics()
                    diagText = t
                    loadingDiag = false
                }
            } label: { Image(systemName: "wave.3.right") }
                .buttonStyle(.borderless)
                .help("Link test: ask the Pi and the DAC what they are")

            Button { WindowPlacer.recenterOnMain() } label: {
                Image(systemName: "macwindow.on.rectangle")
            }
            .buttonStyle(.borderless)
            .help("Move this window to the main display")

            Button { showingSettings = true } label: {
                Image(systemName: "slider.horizontal.3")
            }
            .buttonStyle(.borderless)
            .help("Settings")
        }
        .foregroundStyle(Ink.dim)
    }

    // MARK: Depth — the instrument

    private var depthPanel: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(alignment: .firstTextBaseline, spacing: 10) {
                Text(engine.status.inFlightMs > 0
                     ? String(format: "%.2f", engine.status.inFlightMs) : "—")
                    .font(.system(size: 46, weight: .medium, design: .rounded))
                    .monospacedDigit()
                    .foregroundStyle(engine.isRunning ? Ink.text : Ink.faint)
                Text("ms in flight")
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(Ink.dim)
                Spacer()
                VStack(alignment: .trailing, spacing: 2) {
                    Text(String(format: "ring %.1f", engine.status.ringMs))
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundStyle(Ink.faint)
                    Text(String(format: "dac %.2f–%.2f",
                                engine.status.devP05Ms, engine.status.devP95Ms))
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundStyle(Ink.faint)
                }
            }

            // v1's gauge: a fixed reference bar, no moving history to watch.
            // Fed by the receiver's median fill, so it does not wander.
            FillGauge(fillMs: engine.status.devP50Ms > 0
                          ? engine.status.devP50Ms : engine.status.fillMs,
                      bufferMs: engine.status.bufferMs)

            bufferChoice
        }
        .padding(16)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }

    /// Stepped, not continuous: 2/3/4 ms all clamp to 4 ms in hardware, so a
    /// free slider would offer settings that silently become something else.
    private var bufferChoice: some View {
        HStack(spacing: 6) {
            Text("BUFFER")
                .font(.system(size: 9, weight: .bold, design: .monospaced))
                .tracking(1.0)
                .foregroundStyle(Ink.faint)
            ForEach([4.0, 6.0, 8.0, 12.0, 20.0, 50.0], id: \.self) { ms in
                let selected = abs(engine.bufferMs - ms) < 0.5
                Button { engine.bufferMs = ms } label: {
                    Text("\(Int(ms))")
                        .font(.system(size: 10, weight: selected ? .bold : .regular,
                                      design: .monospaced))
                        .foregroundStyle(selected ? Ink.bg : Ink.dim)
                        .frame(width: 30, height: 20)
                        .background(RoundedRectangle(cornerRadius: 4)
                            .fill(selected ? Ink.accent : Color.white.opacity(0.05)))
                }
                .buttonStyle(.plain)
                .help(ms <= 5
                      ? "\(Int(ms)) ms — the K11's floor, but below the safe margin over ssh"
                      : "\(Int(ms)) ms device buffer")
            }
            Spacer()
            if engine.isRunning {
                Text("takes effect on next play")
                    .font(.system(size: 9, design: .monospaced))
                    .foregroundStyle(Ink.faint)
            }
        }
    }

    // MARK: Side

    @ViewBuilder
    private var sidePanel: some View {
        VStack(alignment: .leading, spacing: 0) {
            nowPlaying
            Divider().overlay(Ink.panelEdge)
            if engine.tracks.isEmpty { logList } else { queueList }
        }
        .frame(width: 330)
    }

    private var nowPlaying: some View {
        let np = engine.nowPlaying
        let hasTags = !np.isEmpty

        return VStack(alignment: .leading, spacing: 7) {
            // Real tags when Spotlight has them, filename when it does not.
            Text(hasTags ? np.title
                         : (file?.lastPathComponent ?? "No track loaded"))
                .font(.system(size: 13, weight: .semibold))
                .lineLimit(1)
                .truncationMode(.tail)
                .foregroundStyle(file == nil ? Ink.dim : Ink.text)

            if hasTags {
                Text(np.artist.isEmpty ? np.album
                                       : (np.album.isEmpty ? np.artist
                                                           : "\(np.artist) — \(np.album)"))
                    .font(.system(size: 11))
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .foregroundStyle(Ink.dim)
            }

            if np.duration > 0 {
                // Elapsed / total, with a thin progress rail. Not scrubbable:
                // the engine has no seek yet, and a bar that looks draggable but
                // is not would be the worst of both.
                HStack(spacing: 7) {
                    Text(clock(engine.status.seconds))
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundStyle(Ink.faint)
                        .frame(width: 34, alignment: .leading)
                    GeometryReader { geo in
                        let t = min(1, max(0, engine.status.seconds / np.duration))
                        ZStack(alignment: .leading) {
                            Capsule().fill(Ink.rail).frame(height: 3)
                            Capsule().fill(Ink.accent.opacity(0.8))
                                .frame(width: geo.size.width * t, height: 3)
                        }
                        .frame(maxHeight: .infinity, alignment: .center)
                    }
                    .frame(height: 10)
                    Text(clock(np.duration))
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundStyle(Ink.faint)
                        .frame(width: 34, alignment: .trailing)
                }
            }

            HStack(spacing: 8) {
                Button { chooseTrack() } label: { Text("Open File") }
                Button { chooseFolder() } label: { Text("Open Folder") }
                    .keyboardShortcut("o", modifiers: [.command])
                Spacer()
            }
            .controlSize(.small)
            .font(.system(size: 11))

            HStack(spacing: 8) {
                Button(engine.dsd ? "DSD256" : "PCM") { engine.dsd.toggle() }
                    .controlSize(.small)
                    .help("Switch between PCM and native DSD256 (applies on next play)")
                Spacer()
                if engine.nowPlaying.bitrate > 0 {
                    Text(String(format: "%.1f Mb/s", Double(np.bitrate) / 1_000_000))
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundStyle(Ink.faint)
                }
            }
        }
        .padding(14)
    }

    private var queueList: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("QUEUE · \(engine.currentTrack) OF \(max(engine.trackTotal, engine.tracks.count))")
                    .font(.system(size: 9, weight: .bold, design: .monospaced))
                    .tracking(1.0)
                    .foregroundStyle(Ink.faint)
                Spacer()
            }
            .padding(.horizontal, 14).padding(.vertical, 9)

            ScrollView {
                LazyVStack(alignment: .leading, spacing: 1) {
                    ForEach(engine.tracks) { t in
                        let active = t.id == engine.currentTrack
                        QueueRow(track: t, active: active, hovered: hovered == t.id)
                            .contentShape(Rectangle())
                            .onHover { inside in
                                hovered = inside ? t.id : (hovered == t.id ? nil : hovered)
                            }
                            .onTapGesture { engine.play(track: t.id) }
                    }
                }
                .padding(.horizontal, 8)
            }
        }
    }

    private var logList: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("ENGINE")
                .font(.system(size: 9, weight: .bold, design: .monospaced))
                .tracking(1.0)
                .foregroundStyle(Ink.faint)
                .padding(.horizontal, 14).padding(.vertical, 9)
            ScrollView {
                VStack(alignment: .leading, spacing: 2) {
                    if engine.log.isEmpty {
                        Text("No activity yet.")
                            .font(.system(size: 10, design: .monospaced))
                            .foregroundStyle(Ink.faint)
                    }
                    ForEach(Array(engine.log.enumerated()), id: \.offset) { _, line in
                        Text(line)
                            .font(.system(size: 10, design: .monospaced))
                            .foregroundStyle(line.contains("underrun") ? Ink.bad : Ink.dim)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                .padding(.horizontal, 14)
            }
        }
    }

    private func clock(_ s: Double) -> String {
        let t = Int(s.rounded())
        return String(format: "%d:%02d", t / 60, t % 60)
    }

    // MARK: Transport

    private var transport: some View {
        HStack(spacing: 12) {
            Button { engine.previous() } label: {
                Image(systemName: "backward.fill").frame(width: 34, height: 26)
            }
            .buttonStyle(.plain)
            .foregroundStyle(Ink.dim)
            .disabled(!engine.isRunning)
            .keyboardShortcut(.leftArrow, modifiers: [.command])
            .help("Previous track (⌘←)")

            Button {
                guard let f = file else { chooseTrack(); return }
                if engine.isRunning { engine.stop() } else { engine.open(url: f) }
            } label: {
                Label(engine.isRunning ? "Stop" : "Play",
                      systemImage: engine.isRunning ? "stop.fill" : "play.fill")
                    .font(.system(size: 12, weight: .semibold))
                    .frame(width: 110, height: 26)
            }
            .buttonStyle(.plain)
            .foregroundStyle(engine.isRunning ? Ink.bg : Ink.bg)
            .background(RoundedRectangle(cornerRadius: 6)
                .fill(engine.isRunning ? Ink.bad : Ink.accent))
            .keyboardShortcut(.space, modifiers: [])
            .help(engine.isRunning ? "Stop (space)" : "Play (space)")

            Button { engine.next() } label: {
                Image(systemName: "forward.fill").frame(width: 34, height: 26)
            }
            .buttonStyle(.plain)
            .foregroundStyle(Ink.dim)
            .disabled(!engine.isRunning)
            .keyboardShortcut(.rightArrow, modifiers: [.command])
            .help("Next track (⌘→)")

            Spacer()

            VolumeKnob(db: Binding(get: { engine.gainDb },
                                   set: { engine.setGain($0) }))

            Divider().frame(height: 26).overlay(Ink.panelEdge)

            RmsMeter(db: engine.status.rmsDb)

            counter(label: "XRUNS",
                    value: "\(engine.status.xruns + engine.status.underruns + engine.status.devXruns)",
                    bad: engine.status.xruns + engine.status.underruns + engine.status.devXruns > 0)

            if let err = engine.errorText {
                Text(err)
                    .font(.system(size: 9, design: .monospaced))
                    .foregroundStyle(Ink.bad)
                    .lineLimit(1)
                    .frame(maxWidth: 260, alignment: .trailing)
            } else if !engine.summary.isEmpty {
                Text(engine.summary)
                    .font(.system(size: 9, design: .monospaced))
                    .foregroundStyle(Ink.faint)
                    .lineLimit(1)
                    .frame(maxWidth: 260, alignment: .trailing)
            }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
        .background(Ink.panel)
    }

    private func counter(label: String, value: String, bad: Bool) -> some View {
        VStack(alignment: .trailing, spacing: 1) {
            Text(label)
                .font(.system(size: 8, weight: .bold, design: .monospaced))
                .tracking(0.8)
                .foregroundStyle(Ink.faint)
            Text(value)
                .font(.system(size: 11, weight: .medium, design: .monospaced))
                .foregroundStyle(bad ? Ink.bad : Ink.dim)
        }
    }

    private var diagSheet: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Link test").font(.headline)
                Spacer()
                Button("Close") { showingDiag = false }
            }
            if loadingDiag {
                ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollView {
                    Text(diagText)
                        .font(.system(size: 11, design: .monospaced))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        }
        .padding(16)
        .frame(width: 560, height: 440)
        .background(Ink.bg)
    }

    private var dropHighlight: some View {
        RoundedRectangle(cornerRadius: 12)
            .strokeBorder(Ink.accent, lineWidth: 2)
            .opacity(dropTargeted ? 1 : 0)
            .padding(6)
            .allowsHitTesting(false)
    }

    private func chooseTrack() { choose(directories: false, prompt: "Play") }

    private func chooseFolder() { choose(directories: true, prompt: "Play Folder") }

    private func choose(directories: Bool, prompt: String) {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = directories
        panel.canChooseFiles = !directories
        panel.canCreateDirectories = false
        panel.prompt = prompt
        panel.message = directories
            ? "Choose a folder of tracks; they play in name order"
            : "Choose a track"
        if panel.runModal() == .OK, let url = panel.url {
            file = url
            engine.open(url: url)
        }
    }

    /// ⌘O / space with nothing loaded.
    private func chooseFile() { chooseFolder() }
}


// MARK: - Components

/// One hop in the signal chain.
struct HopNode: View {
    let label: String
    let value: String
    let sub: String
    let state: HopState

    private var color: Color {
        switch state {
        case .idle: return Ink.faint
        case .good: return Ink.accent
        case .warn: return Ink.warm
        case .bad: return Ink.bad
        }
    }

    var body: some View {
        HStack(spacing: 7) {
            Circle().fill(color).frame(width: 5, height: 5)
            VStack(alignment: .leading, spacing: 1) {
                Text(label)
                    .font(.system(size: 8, weight: .bold, design: .monospaced))
                    .tracking(0.8)
                    .foregroundStyle(Ink.faint)
                Text(value)
                    .font(.system(size: 12, weight: .medium, design: .monospaced))
                    .monospacedDigit()
                    .foregroundStyle(state == .idle ? Ink.faint : Ink.text)
                Text(sub)
                    .font(.system(size: 8, design: .monospaced))
                    .foregroundStyle(Ink.faint)
                    .lineLimit(1)
            }
        }
    }
}

/// A short line between hops, lit while audio is flowing.
struct HopLink: View {
    let active: Bool
    var body: some View {
        Rectangle()
            .fill(active ? Ink.accent.opacity(0.45) : Ink.rail)
            .frame(width: 22, height: 1)
            .padding(.horizontal, 6)
    }
}

/// v1's DAC buffer gauge, kept verbatim: label, one bar, one number.
struct FillGauge: View {
    let fillMs: Double
    let bufferMs: Double

    private var fraction: Double {
        guard bufferMs > 0 else { return 0 }
        return max(0, min(1, fillMs / bufferMs))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text("DAC BUFFER")
                    .font(.system(size: 9, weight: .bold, design: .monospaced))
                    .foregroundStyle(Ink.dim).tracking(1.0)
                Spacer()
                Text(String(format: "%.1f / %.0f ms", fillMs, bufferMs))
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundStyle(Ink.dim)
            }
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 3).fill(Color.black.opacity(0.4))
                    RoundedRectangle(cornerRadius: 3)
                        .fill(LinearGradient(colors: [Ink.accent, Ink.warm],
                                             startPoint: .leading, endPoint: .trailing))
                        .frame(width: geo.size.width * max(fraction, 0))
                }
            }
            .frame(height: 10)
        }
    }
}

/// A real rotary volume control: turn by dragging, double-click to return to
/// unity. Bound to the engine's live gain command, which ramps per block, so
/// turning it during playback does not step or click.
struct VolumeKnob: View {
    @Binding var db: Double
    var range: ClosedRange<Double> = -60.0...6.0

    @State private var dragStartDb: Double? = nil

    private var span: Double { range.upperBound - range.lowerBound }

    /// 0 at the bottom-left of the sweep, 1 at the bottom-right.
    private var fraction: Double {
        max(0, min(1, (db - range.lowerBound) / span))
    }

    private var angle: Double { -135.0 + fraction * 270.0 }

    private var label: String {
        if db <= range.lowerBound { return "−∞" }
        return String(format: "%+.1f", db)
    }

    var body: some View {
        HStack(spacing: 9) {
            ZStack {
                // Track
                Circle()
                    .trim(from: 0, to: 0.75)
                    .stroke(Ink.rail, style: StrokeStyle(lineWidth: 3, lineCap: .round))
                    .rotationEffect(.degrees(135))

                // Filled arc
                Circle()
                    .trim(from: 0, to: 0.75 * fraction)
                    .stroke(Ink.accent,
                            style: StrokeStyle(lineWidth: 3, lineCap: .round))
                    .rotationEffect(.degrees(135))

                // Body
                Circle()
                    .fill(LinearGradient(colors: [Color(white: 0.20), Color(white: 0.12)],
                                         startPoint: .top, endPoint: .bottom))
                    .padding(7)
                    .overlay(Circle().strokeBorder(Ink.panelEdge).padding(7))

                // Pointer
                Rectangle()
                    .fill(Ink.text)
                    .frame(width: 2, height: 7)
                    .offset(y: -8)
                    .rotationEffect(.degrees(angle))
            }
            .frame(width: 40, height: 40)
            .contentShape(Circle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { g in
                        if dragStartDb == nil { dragStartDb = db }
                        // Up increases; 0.6 dB per pixel over a 40 px throw.
                        let delta = -g.translation.height * 0.6
                        var v = (dragStartDb ?? db) + delta
                        if (range.lowerBound...range.upperBound).contains(v) {
                            // Gentle detent at unity, so 0 dB is easy to hit.
                            if abs(v) < 0.6 { v = 0 }
                            db = ((v * 10).rounded()) / 10
                        } else {
                            db = v < range.lowerBound ? range.lowerBound : range.upperBound
                        }
                    }
                    .onEnded { _ in dragStartDb = nil }
            )
            .onTapGesture(count: 2) { db = 0 }
            .help("Volume — drag up/down, double-click for 0 dB")
            .accessibilityLabel("Volume")
            .accessibilityValue("\(label) decibels")

            VStack(alignment: .leading, spacing: 1) {
                Text(label)
                    .font(.system(size: 11, weight: .medium, design: .monospaced))
                    .monospacedDigit()
                    .foregroundStyle(db <= range.lowerBound ? Ink.faint : Ink.text)
                Text("dB")
                    .font(.system(size: 8, design: .monospaced))
                    .foregroundStyle(Ink.faint)
            }
            .frame(width: 44, alignment: .leading)
        }
        .padding(.trailing, 4)
    }
}

/// RMS only. Peak next to it was the other half of "generic audio app"; the
/// number that matters here is level, not transient headroom.
struct RmsMeter: View {
    let db: Double

    var body: some View {
        HStack(spacing: 7) {
            Text("RMS")
                .font(.system(size: 8, weight: .bold, design: .monospaced))
                .tracking(0.8)
                .foregroundStyle(Ink.faint)
            GeometryReader { geo in
                let norm = max(0, min(1, (db + 60) / 60))
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 2).fill(Color.white.opacity(0.06))
                    RoundedRectangle(cornerRadius: 2)
                        .fill(db > -1 ? Ink.bad : (db > -9 ? Ink.warm : Ink.accent))
                        .frame(width: geo.size.width * norm)
                }
            }
            .frame(width: 70, height: 5)
            Text(db <= -119 ? "−∞" : String(format: "%5.1f", db))
                .font(.system(size: 9, design: .monospaced))
                .foregroundStyle(Ink.dim)
                .frame(width: 38, alignment: .trailing)
        }
    }
}

/// One song in the folder. Clicking it jumps playback to that track.
struct QueueRow: View {
    let track: Engine.Track
    let active: Bool
    let hovered: Bool

    var body: some View {
        HStack(spacing: 7) {
            Group {
                if active {
                    Image(systemName: "speaker.wave.2.fill").font(.system(size: 8))
                } else if hovered {
                    Image(systemName: "play.fill").font(.system(size: 8))
                } else {
                    Text(String(format: "%02d", track.id))
                        .font(.system(size: 9, design: .monospaced))
                }
            }
            .foregroundStyle(active ? Ink.accent : (hovered ? Ink.text : Ink.faint))
            .frame(width: 18, alignment: .trailing)

            Text(track.name)
                .font(.system(size: 11, weight: active ? .semibold : .regular))
                .foregroundStyle(active ? Ink.text : (hovered ? Ink.text : Ink.dim))
                .lineLimit(1)
                .truncationMode(.middle)

            Spacer(minLength: 4)
        }
        .padding(.vertical, 3)
        .padding(.horizontal, 5)
        .background(RoundedRectangle(cornerRadius: 4)
            .fill(active ? Ink.accent.opacity(0.12)
                         : (hovered ? Color.white.opacity(0.06) : Color.clear)))
        .help(active ? "Playing" : "Play this track")
    }
}

struct SettingsSheet: View {
    @EnvironmentObject var engine: Engine
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Network settings").font(.headline)
            Form {
                TextField("Host", text: $engine.host, prompt: Text(PiDefaults.host))
                TextField("User", text: $engine.user, prompt: Text("up"))
                TextField("ALSA device", text: $engine.device,
                          prompt: Text(PiDefaults.device))
                TextField("SSH key", text: $engine.keyPath)
                TextField("Receiver", text: $engine.receiver, prompt: Text("aplay"))
            }
            Text("The key must already be in ~/.ssh/authorized_keys on the Pi:\n"
                 + "ssh-copy-id -i <key>.pub \(engine.user)@\(engine.host)")
                .font(.system(size: 10, design: .monospaced))
                .foregroundStyle(Ink.dim)
            Text("Receiver is `aplay` (stock) or a path on the Pi such as ~/hqdac,\n"
                 + "the fixed-buffer engine: it sets the device buffer exactly instead of\n"
                 + "letting aplay fill it, and 4 ms is the K11's floor. Install with\n"
                 + "receiver/install.sh.")
                .font(.system(size: 10, design: .monospaced))
                .foregroundStyle(Ink.dim)
            HStack {
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(18)
        .frame(width: 460)
        .background(Ink.bg)
    }
}
