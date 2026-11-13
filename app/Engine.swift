import Foundation
import Combine

/// Defaults for the rig this was built against, mirroring src/defaults.hpp.
/// Keep the two in step. They make the app usable with no configuration on the
/// machine it was developed on; every one is overridden by the saved setting.
enum PiDefaults {
    static let host = "192.168.86.20"
    static let user = "up"
    static let device = "hw:CARD=R2R,DEV=0"
    static let keyPath = "~/.ssh/hermes_pi"
}

/// Owns the `pibridge` engine process and turns its stderr into UI state.
///
/// The engine is a separate process on purpose: it does the DSP and holds the
/// SSH link, so a UI hang cannot disturb audio, and stopping the app cannot
/// leave a half-written stream behind.
@MainActor
final class Engine: ObservableObject {

    /// The AppDelegate needs to hand files to the same instance the UI is bound
    /// to, and @StateObject cannot be reached from an NSApplicationDelegate.
    static let shared = Engine()

    enum Phase: String {
        case idle, connecting, playing, stopping, failed
    }

    /// Tags for the track on screen. Read once per selection via Spotlight
    /// (mdls), which has them for anything CoreAudio can open and costs ~20 ms.
    struct NowPlaying: Equatable {
        var title: String = ""
        var artist: String = ""
        var album: String = ""
        var duration: Double = 0
        var bitrate: Int = 0

        var isEmpty: Bool { title.isEmpty && artist.isEmpty }
    }

    struct Track: Identifiable, Equatable {
        let id: Int
        let path: String
        var name: String { (path as NSString).lastPathComponent }
    }

    struct Status {
        var fillMs: Double = 0          // one aliased /proc sample; not for display
        var bufferMs: Double = 0        // the device buffer, as the receiver reports it
        var trimPpm: Double = 0
        var underruns: Int = 0
        var xruns: Int = 0
        var frames: Int = 0
        var peakDb: Double = -120
        var rmsDb: Double = -120
        var seconds: Double = 0

        // From the receiver's own windowed DELAY stats (@fill). Min is the one
        // that matters: it is the true worst-case margin, which a single
        // sampled value cannot show.
        var devP05Ms: Double = 0
        var devP50Ms: Double = 0
        var devP95Ms: Double = 0
        var devMinMs: Double = 0
        var devMeanMs: Double = 0
        var devMaxMs: Double = 0
        var ringMs: Double = 0
        var devXruns: Int = 0
        var hasDeviceStats: Bool = false

        var inFlightMs: Double { devMeanMs > 0 ? devMeanMs + ringMs : 0 }

        var formatLabel: String = ""
        var rateLabel: String = ""
    }

    /// One 250 ms window of device-buffer depth, for the trace.

    @Published private(set) var phase: Phase = .idle
    @Published private(set) var status = Status()
    @Published private(set) var log: [String] = []
    @Published private(set) var summary: String = ""
    @Published private(set) var nowPlaying = NowPlaying()
    @Published private(set) var tracks: [Track] = []
    @Published private(set) var currentTrack: Int = 0
    @Published private(set) var trackTotal: Int = 0
    @Published var errorText: String?

    // Settings, persisted. @AppStorage is a View property wrapper and does not
    // drive objectWillChange inside an ObservableObject, so persist by hand.
    @Published var host: String { didSet { store("host", host) } }
    @Published var user: String { didSet { store("user", user) } }
    @Published var device: String { didSet { store("device", device) } }
    @Published var keyPath: String { didSet { store("keyPath", keyPath) } }
    @Published var bufferMs: Double { didSet { store("bufferMs", bufferMs) } }
    /// Remote player. "aplay" is the stock ALSA tool; any other value is an
    /// executable path on the Pi (the included static `hqdac` receiver), which
    /// sets and reports its own device buffer.
    @Published var receiver: String { didSet { store("receiver", receiver) } }
    @Published var dsd: Bool { didSet { store("dsd", dsd) } }
    @Published var gainDb: Double { didSet { store("gainDb", gainDb) } }

    init() {
        let d = UserDefaults.standard
        host = d.string(forKey: "host") ?? PiDefaults.host
        user = d.string(forKey: "user") ?? PiDefaults.user
        device = d.string(forKey: "device") ?? PiDefaults.device
        keyPath = d.string(forKey: "keyPath")
            ?? NSString(string: PiDefaults.keyPath).expandingTildeInPath
        // Migrate once: the old 60 ms default existed only because aplay cannot
        // hold a small buffer. Read the stored values into locals first — reading
        // a property here would go through its didSet before `self` is complete.
        let storedBuffer = d.object(forKey: "bufferMs") as? Double
        let storedReceiver = d.string(forKey: "receiver")
        let era = (storedReceiver == nil && (storedBuffer ?? 0) > 20)
        bufferMs = era ? 8 : (storedBuffer ?? 8)
        receiver = storedReceiver ?? "~/hqdac"
        dsd = d.bool(forKey: "dsd")
        gainDb = d.object(forKey: "gainDb") as? Double ?? 0
        // Assigning during init does not fire didSet, so the migrated values
        // would never reach defaults and the migration would repeat every
        // launch. Pin them once, the first time the receiver is chosen.
        if storedReceiver == nil {
            store("receiver", receiver)
            store("bufferMs", bufferMs)
        }
    }

    private func store(_ key: String, _ value: Any) {
        UserDefaults.standard.set(value, forKey: key)
    }

    private var proc: Process?
    private var stderrPipe: Pipe?
    private var stdoutPipe: Pipe?
    private var stdinPipe: Pipe?
    /// Set when a new selection arrives while a stream is still shutting down.
    private var pendingURL: URL?
    private var tailTimer: Timer?

    var isRunning: Bool { proc?.isRunning ?? false }
    var engineURL: URL? {
        Bundle.main.url(forResource: "pibridge", withExtension: nil)
    }

    // MARK: - Playback

    func play(url: URL) {
        if nowPlaying.isEmpty { loadMetadata(for: url) }
        guard !isRunning else { pendingURL = url; stop(); return }
        pendingURL = nil

        guard let engine = engineURL else {
            errorText = "The pibridge engine is missing from the app bundle."
            phase = .failed
            return
        }

        let isDir = (try? url.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory ?? false
        var args = ["play", url.path,
                    "--host", host,
                    "--user", user,
                    "--device", device,
                    "--key", NSString(string: keyPath).expandingTildeInPath,
                    "--buffer-ms", String(Int(bufferMs)),
                    "--receiver", receiver,
                    "--status-json",
                    "--die-with-parent"]
        if isDir { args += ["--recursive"] }
        if receiver != "aplay" { args += ["--target-ms", String(max(1, Int(bufferMs / 2)))] }
        if dsd { args += ["--mode", "dsd"] }
        if gainDb != 0 { args += ["--gain-db", String(format: "%.1f", gainDb)] }

        let p = Process()
        p.executableURL = engine
        p.arguments = args

        let err = Pipe()
        let out = Pipe()
        p.standardError = err
        p.standardOutput = out
        // stdin is the engine's control channel: "gain <dB>" lines.
        let inPipe = Pipe()
        p.standardInput = inPipe

        status = Status()
        summary = ""
        errorText = nil
        tracks = []
        currentTrack = 0
        trackTotal = 0
        log = ["starting \(url.lastPathComponent)"]
        phase = .connecting

        err.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
            Task { @MainActor [weak self] in self?.ingest(text) }
        }
        out.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return }
            Task { @MainActor [weak self] in self?.ingest(text) }
        }

        p.terminationHandler = { [weak self] finished in
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.finish(exitCode: finished.terminationStatus)
            }
        }

        do {
            try p.run()
            proc = p
            stderrPipe = err
            stdoutPipe = out
            stdinPipe = inPipe
        } catch {
            errorText = "Could not start the engine: \(error.localizedDescription)"
            phase = .failed
        }
    }

    /// Reads tags for the selected file. Falls back to the filename when
    /// Spotlight has nothing, which is normal for files it has not indexed.
    func loadMetadata(for url: URL) {
        let isDir = (try? url.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory ?? false
        guard !isDir else { nowPlaying = NowPlaying(); return }

        DispatchQueue.global(qos: .userInitiated).async {
            var np = NowPlaying()
            let p = Process()
            p.executableURL = URL(fileURLWithPath: "/usr/bin/mdls")
            p.arguments = ["-name", "kMDItemTitle", "-name", "kMDItemAuthors",
                           "-name", "kMDItemAlbum", "-name", "kMDItemDurationSeconds",
                           "-name", "kMDItemAudioBitRate", url.path]
            let pipe = Pipe()
            p.standardOutput = pipe
            p.standardError = FileHandle.nullDevice
            do {
                try p.run()
            } catch {
                Task { @MainActor in self.nowPlaying = NowPlaying() }
                return
            }
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            p.waitUntilExit()
            let text = String(data: data, encoding: .utf8) ?? ""

            // mdls prints multi-line plist arrays, so flatten to one line and
            // split by key. Artists in particular arrives as ( "A", "B" ).
            let flat = text.split(separator: "\n").joined(separator: " ")
            func field(_ key: String) -> String? {
                // Keys are space-padded up to "=" and values are quoted, e.g.
                //   kMDItemTitle           = "The Night We Called It A Day"
                // so locate the key, then the next "=" after it, not "key = ".
                guard let kr = flat.range(of: key) else { return nil }
                let afterKey = flat[kr.upperBound...]
                guard let eq = afterKey.firstIndex(of: "=") else { return nil }
                let rest = afterKey[afterKey.index(after: eq)...]
                // Value ends where the next key begins.
                let end = rest.range(of: "kMDItem")?.lowerBound ?? rest.endIndex
                var v = String(rest[..<end])
                v = v.replacingOccurrences(of: "(", with: "")
                    .replacingOccurrences(of: ")", with: "")
                    .replacingOccurrences(of: "\"", with: "")
                    .trimmingCharacters(in: .whitespaces)
                if v.isEmpty || v == "null" { return nil }
                return v
            }

            np.title = field("kMDItemTitle") ?? ""
            np.album = field("kMDItemAlbum") ?? ""
            // Authors arrives as a plist array flattened to "A", "B".
            if let authors = field("kMDItemAuthors") {
                let cleaned = authors
                    .split(separator: ",")
                    .map { $0.trimmingCharacters(in: .whitespaces) }
                    .filter { !$0.isEmpty }
                np.artist = cleaned.joined(separator: ", ")
            }
            if let d = field("kMDItemDurationSeconds"), let v = Double(d) { np.duration = v }
            if let b = field("kMDItemAudioBitRate") { np.bitrate = Int(Double(b) ?? 0) }

            Task { @MainActor in self.nowPlaying = np }
        }
    }

    /// Finder "Open With", drag onto the Dock icon, or `open -a PiBridge <file>`.
    func handleOpen(url: URL) { open(url: url) }

    /// Plays `url`, replacing whatever is playing. stop() is asynchronous, so
    /// the request is queued and started when the old process has really gone;
    /// otherwise the new file would be silently dropped.
    func open(url: URL) {
        loadMetadata(for: url)
        guard isRunning else { play(url: url); return }
        pendingURL = url
        stop()
    }

    func stop() {
        guard let p = proc else {
            // Nothing to stop: if something was queued behind a stopping
            // process, start it now rather than dropping it.
            if let queued = pendingURL {
                pendingURL = nil
                play(url: queued)
            }
            return
        }
        if p.isRunning {
            phase = .stopping
            p.interrupt()   // SIGINT: the engine drains the DAC buffer and reports
        }
        // If the process already died, terminationHandler fired or is about to;
        // the watchdog below covers the case where it never does.
        let expected = p
        DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) { [weak self] in
            guard let self, self.pendingURL != nil, self.proc === expected,
                  !expected.isRunning else { return }
            self.proc = nil
            if let queued = self.pendingURL {
                self.pendingURL = nil
                self.play(url: queued)
            }
        }
    }

    // MARK: - Output handling

    private func ingest(_ text: String) {
        for line in text.split(separator: "\n", omittingEmptySubsequences: true) {
            let s = String(line)
            if s.hasPrefix("@item ") {
                applyItem(json: String(s.dropFirst("@item ".count)))
            } else if s.hasPrefix("@track ") {
                applyTrack(json: String(s.dropFirst("@track ".count)))
            } else if s.hasPrefix("@status ") {
                apply(json: String(s.dropFirst("@status ".count)))
            } else if s.hasPrefix("@fill ") {
                applyFill(json: String(s.dropFirst("@fill ".count)))
            } else if s.hasPrefix("pibridge: ") {
                let msg = String(s.dropFirst("pibridge: ".count))
                note(msg)
            }
        }
    }

    /// A track in the folder, announced before playback so the whole album is
    /// listed and every row is selectable.
    private func applyItem(json: String) {
        guard let data = json.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let index = (obj["index"] as? NSNumber)?.intValue,
              let total = (obj["total"] as? NSNumber)?.intValue,
              let path = obj["path"] as? String else { return }

        if trackTotal != total { trackTotal = total }
        if let at = tracks.firstIndex(where: { $0.id == index }) {
            tracks[at] = Track(id: index, path: path)
        } else {
            // Items arrive in order, so appending is enough in practice, but
            // sort keeps a truncated or out-of-order queue sane.
            tracks.append(Track(id: index, path: path))
            if let last = tracks.last, last.id != tracks.count { tracks.sort { $0.id < $1.id } }
        }
    }

    /// Jump to a track by 1-based queue position.
    func play(track index: Int) {
        guard index > 0, index <= max(trackTotal, tracks.count) else { return }
        currentTrack = index
        send("play \(index)")
    }

    private func applyTrack(json: String) {
        guard let data = json.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let index = (obj["index"] as? NSNumber)?.intValue,
              let total = (obj["total"] as? NSNumber)?.intValue,
              let path = obj["path"] as? String else { return }

        // Append only the tracks the engine has actually announced. Building a
        // placeholder array of `total` rows made a 1,000-track folder allocate
        // and lay out a thousand views on the main thread — that was the beach
        // ball. `total` is kept for the "n of N" label.
        trackTotal = total
        if let at = tracks.firstIndex(where: { $0.id == index }) {
            tracks[at] = Track(id: index, path: path)
        } else {
            tracks.append(Track(id: index, path: path))
            tracks.sort { $0.id < $1.id }
        }
        currentTrack = index
    }

    private func apply(json: String) {
        guard let data = json.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return
        }
        func d(_ k: String) -> Double { (obj[k] as? NSNumber)?.doubleValue ?? 0 }
        var s = status
        s.fillMs = d("fill_ms")
        s.bufferMs = d("buffer_ms")
        s.trimPpm = d("trim_ppm")
        s.underruns = Int(d("underruns"))
        s.xruns = Int(d("xruns"))
        s.frames = Int(d("frames"))
        s.peakDb = d("peak_db")
        s.rmsDb = d("rms_db")
        s.seconds = d("t")
        status = s
        if phase == .connecting { phase = .playing }
    }

    /// A window of real device-buffer depth from the receiver.
    private func applyFill(json: String) {
        guard let data = json.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return
        }
        func d(_ k: String) -> Double { (obj[k] as? NSNumber)?.doubleValue ?? 0 }
        var st = status
        st.devMinMs = d("min_ms")
        st.devMeanMs = d("mean_ms")
        st.devMaxMs = d("max_ms")
        st.devP05Ms = d("p05_ms")
        st.devP50Ms = d("p50_ms")
        st.devP95Ms = d("p95_ms")
        st.ringMs = d("ring_ms")
        st.devXruns = Int(d("dev_xruns"))
        if d("buffer_ms") > 0 { st.bufferMs = d("buffer_ms") }
        st.hasDeviceStats = true
        status = st

        if phase == .connecting { phase = .playing }
    }

    private func note(_ msg: String) {
        // Keep the log short; the counters live in the meters.
        let lower = msg.lowercased()
        if lower.contains("underrun") || lower.contains("error") ||
            msg.contains("aplay said") || msg.contains("closed") ||
            lower.contains("already in use") || lower.contains("still held") ||
            lower.contains("receiver stopped") {
            errorText = msg
        }
        log.append(msg)
        if log.count > 200 { log.removeFirst(log.count - 200) }
        if phase == .connecting, msg.contains("streaming") { phase = .playing }
        if let arrow = msg.range(of: " -> ") {
            // "… -> hw:CARD=R2R,DEV=0 S32_LE 384000 Hz, buffer 8 ms, target 4 ms"
            let rest = msg[arrow.upperBound...]
            let bits = rest.split(separator: " ")
            if bits.count >= 3, bits[1].contains("_") || bits[1] == "DSD_U32_BE" {
                var st = status
                st.formatLabel = String(bits[1])
                if let hz = Double(bits[2]) {
                    st.rateLabel = String(format: "%.1f kHz", hz / 1000)
                }
                status = st
            }
        }
    }

    /// Live gain. Sends the value to the running engine; the engine ramps to it,
    /// so this is safe to call while audio is playing.
    func setGain(_ db: Double) {
        gainDb = db
        guard let pipe = stdinPipe else { return }
        let cmd = String(format: "gain %.2f\n", db)
        pipe.fileHandleForWriting.write(Data(cmd.utf8))
    }

    func next() { send("next") }
    func previous() { send("prev") }

    /// Called when the app is terminating. Sends `quit`, waits briefly for the
    /// engine to drain and release the DAC, then forces it. Without this the
    /// engine was orphaned and kept playing after the window closed.
    func shutdown() {
        guard let p = proc else { return }
        if p.isRunning {
            send("quit")
            let deadline = Date().addingTimeInterval(1.5)
            while p.isRunning && Date() < deadline {
                usleep(50_000)
            }
            if p.isRunning { p.terminate() }
            let hard = Date().addingTimeInterval(1.0)
            while p.isRunning && Date() < hard {
                usleep(50_000)
            }
            if p.isRunning { kill(p.processIdentifier, SIGKILL) }
        }
        try? stdinPipe?.fileHandleForWriting.close()
        proc = nil
        stdinPipe = nil
    }

    private func send(_ command: String) {
        guard let pipe = stdinPipe else { return }
        pipe.fileHandleForWriting.write(Data((command + "\n").utf8))
    }

    private func finish(exitCode: Int32) {
        stdinPipe = nil
        proc = nil
        stderrPipe?.fileHandleForReading.readabilityHandler = nil
        stdoutPipe?.fileHandleForReading.readabilityHandler = nil
        try? stdinPipe?.fileHandleForWriting.close()
        stderrPipe = nil
        stdoutPipe = nil

        if phase != .failed { phase = exitCode == 0 ? .idle : .failed }
        if exitCode != 0 && errorText == nil {
            errorText = "Engine exited with code \(exitCode)."
        }
        let bad = status.xruns + status.underruns
        if let queued = pendingURL {
            pendingURL = nil
            play(url: queued)
            return
        }

        summary = String(format: "played %@ in %.1f s · fill %.1f ms · %d xrun%@",
                         timeString(status.seconds), status.seconds,
                         status.fillMs, bad, bad == 1 ? "" : "s")
    }

    private func timeString(_ s: Double) -> String {
        let total = Int(s.rounded())
        return String(format: "%d:%02d", total / 60, total % 60)
    }

    // MARK: - Diagnostics

    func runDiagnostics() async -> String {
        guard let engine = engineURL else { return "engine binary missing" }
        return await withCheckedContinuation { cont in
            let p = Process()
            p.executableURL = engine
            p.arguments = ["diag", "--host", host, "--user", user,
                           "--key", NSString(string: keyPath).expandingTildeInPath]
            let pipe = Pipe()
            p.standardOutput = pipe
            p.standardError = pipe
            do {
                try p.run()
            } catch {
                cont.resume(returning: "could not run: \(error.localizedDescription)")
                return
            }
            DispatchQueue.global().async {
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                p.waitUntilExit()
                let text = String(data: data, encoding: .utf8) ?? "(no output)"
                cont.resume(returning: text)
            }
        }
    }
}
