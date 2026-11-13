// pibridge — real-time PCM / native-DSD streaming from this Mac to the
// Raspberry Pi's ALSA device, with closed-loop correction of the clock
// difference between the Mac and the DAC.
//
//   pibridge play FILE [options]
//
// The audio path is:  decode -> windowed-sinc ASRC -> [PCM S32_LE | 1-bit SDM]
//                     -> ssh channel -> aplay -> FiiO K11 R2R
//
// The Pi's ALSA buffer depth is read back over a second channel on the same
// multiplexed SSH connection and fed to a PI controller that trims the ASRC
// ratio by a few parts per million. That removes the drift that otherwise
// forces a huge buffer: the transport runs at the DAC's clock, not the Mac's.

#include <getopt.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <map>
#include <sys/stat.h>
#include <sys/wait.h>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "asrc.hpp"
#include "dsd.hpp"
#include "defaults.hpp"
#include "link.hpp"
#include "wav.hpp"

using namespace pib;

namespace {

volatile sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void sleepUntil(double t) {
    for (;;) {
        const double left = t - nowSeconds();
        if (left <= 0.0) return;
        std::this_thread::sleep_for(std::chrono::duration<double>(left));
    }
}

struct Options {
    std::string file;                 // first input, for display
    std::vector<std::string> inputs;  // files and/or directories, in order
    std::string host = kDefaultHost;
    std::string user = kDefaultUser;
    std::string key;
    std::string sock;
    std::string device = kDefaultDevice;
    std::string receiver = "aplay";     // remote player: aplay | /path/to/hqdac
    std::string mode = "pcm";          // pcm | dsd
    std::string localOut;              // write raw output to this file instead of the Pi
    std::string metricsPath;
    double outRate = 0.0;
    double bufferMs = 60.0;
    double targetMs = 100.0;
    double gainDb = 0.0;
    double maxPpm = 500.0;
    double blockMs = 0.0;   // 0 = auto
    std::string pace = "clock";  // clock | backpressure
    bool paceExplicit = false;
    int taps = 192;
    int phases = 512;
    bool verbose = false;
    bool steer = false;
    bool recursive = false;
    bool dieWithParent = false;
    bool statusJson = false;
};

// ---------------------------------------------------------------------------
// Telemetry from the Pi: one line per poll, "S <state> <delay-frames>".
// ---------------------------------------------------------------------------
// /tmp path for this engine's receiver telemetry.
inline std::string statsPathForPid() {
    return "/tmp/hqdac." + std::to_string(static_cast<long>(::getpid())) + ".stats";
}

struct Telemetry {
    std::atomic<long> delay{0};
    std::atomic<int> stateCode{0};  // 0 none, 1 running, 2 draining, 3 other
    std::atomic<bool> have{false};
    std::atomic<long> polls{0};
    // The receiver's own windowed min/max/mean of the device delay. This is
    // what the GUI graphs: our 80 ms poll of /proc aliases against a 4-8 ms
    // buffer and reports phantom minimums, so it is not trustworthy on its own.
    std::atomic<long> devMin{-1}, devMax{-1}, devMean{-1};   // frames
    std::atomic<long> devP05{-1}, devP50{-1}, devP95{-1};    // frames
    std::atomic<long> devRun{-1};                            // consecutive low polls
    std::atomic<long> devXruns{-1};
    std::atomic<long> devRate{0}, devBuffer{0}, devRing{0};
    std::atomic<long> windows{0};
    std::atomic<long> devSeq{0};   // receiver's own window counter
};

bool spawnTelemetry(const Options& o, Child& child, std::string& err) {
    // Where hqdac publishes its windowed device-buffer stats. Named after this
    // process: a leaked receiver from an earlier run otherwise keeps writing to
    // a shared file and the new stream graphs the old one's numbers.
    const std::string statsPath = statsPathForPid();
    // Single-quoted so nothing expands on this side; the remote shell runs it.
    // This loop outlives its ssh client if pibridge is killed (SIGKILL leaves no
    // chance to reap the channel), and three of them were found still spinning
    // after earlier runs. It now exits by itself once the device has been
    // closed for a couple of seconds, so an orphan cannot poll forever.
    const std::string script =
        "gone=0; "
        "while :; do "
        "s=$(cat /proc/asound/card0/pcm0p/sub0/status 2>/dev/null); "
        "st=$(printf \"%s\\n\" \"$s\" | sed -n \"s/^state: *\\([A-Z]*\\).*/\\1/p\"); "
        "d=$(printf \"%s\\n\" \"$s\" | sed -n \"s/^delay *: *\\([0-9]*\\).*/\\1/p\"); "
        "echo \"S ${st:-none} ${d:-0}\"; "
        "f=$(cat " + statsPath + " 2>/dev/null | tr \"\\n\" \" \"); "
        "[ -n \"$f\" ] && echo \"F $f\"; "
        "if [ \"${st:-none}\" = \"closed\" ]; then gone=$((gone+1)); else gone=0; fi; "
        "[ $gone -gt 25 ] && break; "
        "sleep 0.08; done";
    const std::string remote = "sh -c '" + script + "'";
    return spawnRemote(o.sock, o.key, o.user, o.host, remote, false, true, false, child, err);
}

// aplay prints "underrun!!!" on every xrun, as it happens. Reading its stderr
// only at the end of playback (and counting 0/1) hid exactly the defect a
// listener hears: sporadic clicks that never show up in the polled counters.
struct StderrWatch {
    std::atomic<long> xruns{0};
    std::mutex mu;
    std::string text;

    void feed(const char* data, std::size_t n) {
        std::string chunk(data, n);
        long found = 0;
        std::size_t at = 0;
        while ((at = chunk.find("underrun", at)) != std::string::npos) { ++found; at += 8; }
        if (found) xruns.fetch_add(found, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mu);
        text += chunk;
        if (text.size() > 8192) text.erase(0, text.size() - 8192);
    }
};

void stderrThread(int fd, StderrWatch* w) {
    char buf[2048];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) break;
        w->feed(buf, static_cast<std::size_t>(n));
    }
}

// stdin control channel: "gain <dB>", "quit". Read on its own thread so a
// missing writer can never block playback.
void controlThread(std::atomic<double>* gainTarget, std::atomic<bool>* quit,
                   std::atomic<int>* skip, std::atomic<int>* jumpTo, bool dieWithParent) {
    std::string acc;
    char buf[256];
    for (;;) {
        const ssize_t n = ::read(0, buf, sizeof buf);
        if (n <= 0) {
            // The launching app holds the write end of this pipe. EOF means it
            // is gone, and an orphaned engine would keep the DAC busy and keep
            // playing music after the window closed. That was a real bug.
            if (dieWithParent) quit->store(true, std::memory_order_relaxed);
            return;
        }
        acc.append(buf, static_cast<std::size_t>(n));
        std::size_t nl;
        while ((nl = acc.find('\n')) != std::string::npos) {
            const std::string line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            double db = 0.0;
            if (std::sscanf(line.c_str(), "gain %lf", &db) == 1) {
                if (db < -60.0) db = -60.0;
                if (db > 12.0) db = 12.0;
                gainTarget->store(std::pow(10.0, db / 20.0), std::memory_order_relaxed);
            } else if (line == "quit" || line == "stop") {
                quit->store(true, std::memory_order_relaxed);
            } else if (line == "next") {
                skip->store(1, std::memory_order_relaxed);
            } else if (line == "prev") {
                skip->store(-1, std::memory_order_relaxed);
            } else if (line.compare(0, 5, "play ") == 0) {
                // 1-based track number from the queue view. 0 means "none".
                const int want = std::atoi(line.c_str() + 5);
                if (want > 0) jumpTo->store(want - 1, std::memory_order_relaxed);
            }
        }
    }
}

// If the parent is gone we are reparented to launchd (ppid 1). A pipe EOF
// normally catches this first; this covers the case where stdin was /dev/null.
void parentWatchdog(pid_t parent, std::atomic<bool>* quit) {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (quit->load(std::memory_order_relaxed)) return;
        if (::getppid() != parent) {
            quit->store(true, std::memory_order_relaxed);
            return;
        }
    }
}

int stateCodeOf(const std::string& s) {
    if (s == "RUNNING") return 1;
    if (s == "DRAINING") return 2;
    if (s == "none") return 0;
    return 3;
}

void telemetryThread(Child child, Telemetry* t) {
    std::string acc;
    char buf[512];
    for (;;) {
        const ssize_t n = ::read(child.out, buf, sizeof buf);
        if (n <= 0) break;
        acc.append(buf, static_cast<std::size_t>(n));
        std::size_t nl;
        while ((nl = acc.find('\n')) != std::string::npos) {
            const std::string line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            long delay = 0;
            char state[32] = {0};
            if (std::sscanf(line.c_str(), "S %31s %ld", state, &delay) == 2) {
                t->delay.store(delay, std::memory_order_relaxed);
                t->stateCode.store(stateCodeOf(state), std::memory_order_relaxed);
                t->have.store(true, std::memory_order_relaxed);
                t->polls.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            long wMin = 0, wMax = 0, wMean = 0, wXr = 0, wRate = 0, wBuf = 0, wRing = 0;
            long long wFrames = 0;
            long wSeq = 0, wP05 = 0, wP50 = 0, wP95 = 0, wRun = 0;
            // "F min 1152 max 1536 mean 1534 xruns 0 rate 384000 buffer 1536
            //  ring 4096 frames 576480"
            if (std::sscanf(line.c_str(),
                            "F min %ld max %ld mean %ld xruns %ld rate %ld buffer %ld ring %ld "
                            "frames %lld seq %ld p05 %ld p50 %ld p95 %ld run %ld",
                            &wMin, &wMax, &wMean, &wXr, &wRate, &wBuf, &wRing, &wFrames, &wSeq,
                            &wP05, &wP50, &wP95, &wRun) == 13) {
                t->devMin.store(wMin, std::memory_order_relaxed);
                t->devMax.store(wMax, std::memory_order_relaxed);
                t->devMean.store(wMean, std::memory_order_relaxed);
                t->devXruns.store(wXr, std::memory_order_relaxed);
                t->devRate.store(wRate, std::memory_order_relaxed);
                t->devBuffer.store(wBuf, std::memory_order_relaxed);
                t->devRing.store(wRing, std::memory_order_relaxed);
                t->devP05.store(wP05, std::memory_order_relaxed);
                t->devP50.store(wP50, std::memory_order_relaxed);
                t->devP95.store(wP95, std::memory_order_relaxed);
                t->devRun.store(wRun, std::memory_order_relaxed);
                t->devSeq.store(wSeq, std::memory_order_relaxed);
                t->windows.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

// ---------------------------------------------------------------------------

struct Stats {
    std::vector<double> delayMs;
    std::vector<double> ppm;
    long underruns = 0;
    long minDelay = 1L << 40;
    long maxDelay = 0;
    double seconds = 0.0;
};

double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (double x : v) s += x;
    return s / static_cast<double>(v.size());
}

double stddev(const std::vector<double>& v) {
    if (v.size() < 2) return 0.0;
    const double m = mean(v);
    double s = 0.0;
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt(s / static_cast<double>(v.size() - 1));
}

bool writeAll(int fd, const void* data, std::size_t len) {
    const char* p = static_cast<const char*>(data);
    std::size_t left = len;
    while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    return true;
}

// Natural ordering so "track 2" sorts before "track 10".
bool naturalLess(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = std::isdigit(static_cast<unsigned char>(a[i]));
        const bool db = std::isdigit(static_cast<unsigned char>(b[j]));
        if (da && db) {
            std::size_t i2 = i, j2 = j;
            while (i2 < a.size() && std::isdigit(static_cast<unsigned char>(a[i2]))) ++i2;
            while (j2 < b.size() && std::isdigit(static_cast<unsigned char>(b[j2]))) ++j2;
            const long long na = std::atoll(a.substr(i, i2 - i).c_str());
            const long long nb = std::atoll(b.substr(j, j2 - j).c_str());
            if (na != nb) return na < nb;
            i = i2;
            j = j2;
            continue;
        }
        const char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        const char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[j])));
        if (ca != cb) return ca < cb;
        ++i;
        ++j;
    }
    return a.size() < b.size();
}

bool playableExtension(const std::string& path) {
    static const char* exts[] = {".wav", ".flac", ".aif", ".aiff", ".m4a", ".mp3",
                                 ".aac", ".alac", ".caf", ".aifc", ".mp4", ".dsf"};
    std::string lower = path;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* e : exts) {
        const std::size_t n = std::strlen(e);
        if (lower.size() > n && lower.compare(lower.size() - n, n, e) == 0) return true;
    }
    return false;
}

// Expands directories into a sorted file list, preserving argument order.
std::vector<std::string> collectTracks(const std::vector<std::string>& inputs,
                                       bool recursive, std::string& err) {
    std::vector<std::string> out;
    namespace fs = std::filesystem;
    for (const auto& in : inputs) {
        std::error_code ec;
        if (fs::is_directory(in, ec)) {
            std::vector<std::string> found;
            if (recursive) {
                for (auto it = fs::recursive_directory_iterator(in, ec);
                     it != fs::recursive_directory_iterator(); it.increment(ec)) {
                    if (ec) break;
                    if (it->is_regular_file(ec) && playableExtension(it->path().string()))
                        found.push_back(it->path().string());
                }
            } else {
                for (auto it = fs::directory_iterator(in, ec);
                     it != fs::directory_iterator(); it.increment(ec)) {
                    if (ec) break;
                    if (it->is_regular_file(ec) && playableExtension(it->path().string()))
                        found.push_back(it->path().string());
                }
            }
            std::sort(found.begin(), found.end(), naturalLess);
            out.insert(out.end(), found.begin(), found.end());
        } else if (fs::is_regular_file(in, ec)) {
            out.push_back(in);
        } else {
            err = "no such file or directory: " + in;
        }
    }
    return out;
}

bool isWav(const std::string& path) {
    std::string lower = path;
    for (auto& c : lower) c = static_cast<char>(std::tolower(c));
    return lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".wav") == 0;
}

// Decode scratch files are created per process and removed on exit. Without
// this, a killed run leaves a whole track's worth of PCM in /tmp.
std::vector<std::string>& scratchFiles() {
    static std::vector<std::string> files;
    return files;
}

void removeScratch() {
    for (const auto& f : scratchFiles()) ::unlink(f.c_str());
    scratchFiles().clear();
}

// One background transcode per track, so playback can start on the first
// seconds instead of waiting for the whole file.
struct Transcode {
    pid_t pid = -1;
    std::string path;
    std::atomic<bool> done{false};
};
std::vector<std::unique_ptr<Transcode>>& transcodes() {
    static std::vector<std::unique_ptr<Transcode>> v;
    return v;
}
// input path -> its transcode, so a file is decoded once per process.
std::map<std::string, Transcode*>& transcodeCache() {
    static std::map<std::string, Transcode*> m;
    return m;
}

// Waits until the WAV has a parseable header and at least `seconds` of audio,
// or the writer exits. Returns false only if nothing usable ever appeared.
bool waitForAudio(const std::string& path, double rate, int ch, int bits, double seconds,
                  const std::atomic<bool>& done, std::string& err) {
    const long frameBytes = static_cast<long>(bits / 8) * ch;
    const long headerGuess = 4096;   // generous: afconvert writes an extended fmt
    const long need = headerGuess + static_cast<long>(rate * seconds) * frameBytes;
    const double deadline = nowSeconds() + 15.0;
    for (;;) {
        struct stat st;
        if (::stat(path.c_str(), &st) == 0 && static_cast<long>(st.st_size) >= need) return true;
        if (done.load(std::memory_order_relaxed)) {
            // Finished: as long as there is *some* audio beyond the header, go.
            if (::stat(path.c_str(), &st) == 0 && st.st_size > headerGuess) return true;
            err = "decoder produced no audio";
            return false;
        }
        if (nowSeconds() > deadline) {
            err = "timed out waiting for the decoder";
            return false;
        }
        ::usleep(5000);
    }
}

// Waits until enough of the file exists to read its WAV header.
bool waitForHeader(const std::string& path, const std::atomic<bool>& done, std::string& err) {
    const double deadline = nowSeconds() + 15.0;
    for (;;) {
        struct stat st;
        if (::stat(path.c_str(), &st) == 0 && st.st_size >= 4096) return true;
        if (done.load(std::memory_order_relaxed)) {
            if (::stat(path.c_str(), &st) == 0 && st.st_size > 0) return true;
            err = "decoder produced no output";
            return false;
        }
        if (nowSeconds() > deadline) {
            err = "timed out waiting for the decoder";
            return false;
        }
        ::usleep(5000);
    }
}

// Decodes anything CoreAudio understands into a float32 WAV, in the background.
bool materialise(const std::string& in, std::string& out, std::string& err,
                 const std::atomic<bool>** doneFlag) {
    if (isWav(in)) {
        out = in;
        if (doneFlag) *doneFlag = nullptr;   // already complete
        return true;
    }
    auto cached = transcodeCache().find(in);
    if (cached != transcodeCache().end()) {
        out = cached->second->path;
        if (doneFlag) *doneFlag = &cached->second->done;
        return true;
    }
    auto t = std::make_unique<Transcode>();
    t->path = "/tmp/pibridge-decode-" + std::to_string(::getpid()) + "-" +
              std::to_string(static_cast<long>(transcodes().size())) + ".wav";
    out = t->path;
    scratchFiles().push_back(out);

    t->pid = ::fork();
    if (t->pid == 0) {
        ::execl("/usr/bin/afconvert", "afconvert", "-f", "WAVE", "-d", "LEF32",
                in.c_str(), out.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    if (t->pid < 0) {
        err = "cannot start afconvert";
        return false;
    }
    Transcode* raw = t.get();
    transcodes().push_back(std::move(t));
    transcodeCache()[in] = raw;
    if (doneFlag) *doneFlag = &raw->done;

    // Reaper: marks the flag the reader polls for real EOF. Detached so the
    // streaming loop never blocks on the decoder.
    std::thread([raw]() {
        int status = 0;
        ::waitpid(raw->pid, &status, 0);
        raw->done.store(true, std::memory_order_relaxed);
    }).detach();
    return true;
}

std::string formatName(const Options& o) { return o.mode == "dsd" ? "DSD_U32_BE" : "S32_LE"; }

void usage() {
    std::printf(
        "pibridge play FILE|DIR [FILE|DIR ...] [options]\n"
        "pibridge diag [--host H] [--user U] [--key P]\n"
        "\n"
        "  --host HOST        Pi address            (default %s)\n"
        "  --user USER        ssh user              (default %s)\n"
        "  --key PATH         ssh identity file     (default ~/.ssh/%s)\n"
        "  --sock PATH        control socket        (default /tmp/pibridge.sock)\n"
        "  --device DEV       ALSA device           (default %s)\n"
        "  --receiver CMD     remote player         (default aplay; e.g. ~/hqdac)\n"
        "  --mode pcm|dsd     output format         (default pcm)\n"
        "  --rate HZ          output rate/word-rate (default: 384000 pcm, 352800 dsd)\n"
        "  --buffer-ms N      ALSA buffer on the Pi (default 60)\n"
        "  --target-ms N      prefill depth         (default half the buffer)\n"
        "  --gain-db N        pre-quantiser gain    (default 0)\n"
        "  --block-ms N       write block size      (default min(10, buffer/4))\n"
        "  --taps N           ASRC kernel length    (default 192)\n"
        "  --phases N         kernel table phases   (default 512)\n"
        "  --max-ppm N        drift correction cap  (default 500)\n"
        "  --block-ms N       write block            (default 20)\n"
        "  --steer            enable closed-loop drift correction (experimental)\n"
        "  -R, --recursive    descend into sub-folders of a directory\n"
        "  --die-with-parent  exit when the launching process goes away\n"
        "  --pace MODE        backpressure | clock   (default backpressure)\n"
        "  --local-out FILE   write raw output locally instead of streaming\n"
        "  --metrics FILE     write a JSON report\n"
        "  -v, --verbose      per-second status\n"
        "  --status-json      per-second @status JSON lines on stderr\n",
        kDefaultHost, kDefaultUser, kDefaultKeyName, kDefaultDevice);
}

}  // namespace

int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);
    std::atexit(removeScratch);
    ::signal(SIGINT, onSignal);
    ::signal(SIGTERM, onSignal);

    if (argc >= 2 && std::strcmp(argv[1], "diag") == 0) {
        std::string host = kDefaultHost, user = kDefaultUser, key, sock = "/tmp/pibridge.sock";
        for (int i = 2; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
            else if (!std::strcmp(argv[i], "--user") && i + 1 < argc) user = argv[++i];
            else if (!std::strcmp(argv[i], "--key") && i + 1 < argc) key = argv[++i];
        }
        if (key.empty()) key = defaultKeyPath();
        std::string err;
        if (!ensureMaster(sock, key, user, host, err)) {
            std::fprintf(stderr, "pibridge diag: %s\n", err.c_str());
            return 1;
        }
        const std::string remote =
            "echo \"host: $(uname -srm)\"; "
            "echo \"model: $(cat /proc/device-tree/model 2>/dev/null | tr -d '\\0')\"; "
            "echo \"cores: $(nproc)\"; "
            "echo; echo \"--- sound cards\"; cat /proc/asound/cards; "
            "echo; echo \"--- formats\"; "
            "aplay -D hw:CARD=R2R,DEV=0 -t raw -f S32_LE -r 44100 -c 2 --dump-hw-params - </dev/null 2>&1 "
            "| sed -n '/FORMAT/p;/RATE/p;/BUFFER_SIZE/p'; "
            "echo; echo \"--- DSD altset\"; "
            "sed -n '/DSD_U32_BE/,+3p' /proc/asound/card0/stream0 | head -n 4; "
            "echo; echo \"--- receiver\"; "
            "for r in ~/hqdac /tmp/hqdac; do "
            "if [ -x $r ]; then echo \"found: $r\"; "
            "$r -D hw:CARD=R2R,DEV=0 --caps 2>&1 | sed -n \"1p\"; break; fi; done; "
            "echo; echo \"--- link\"; "
            "for i in /sys/class/net/*; do n=$(basename $i); "
            "sp=$(cat $i/speed 2>/dev/null); "
            "[ -n \"$sp\" ] && echo \"$n $sp Mb/s\"; done; true";
        Child c;
        if (!spawnRemote(sock, key, user, host, remote, false, true, true, c, err)) {
            std::fprintf(stderr, "pibridge diag: %s\n", err.c_str());
            return 1;
        }
        char b[8192];
        for (;;) {
            const ssize_t n = ::read(c.out, b, sizeof b);
            if (n <= 0) break;
            ::fwrite(b, 1, static_cast<std::size_t>(n), stdout);
        }
        c.reap();
        return 0;
    }

    if (argc < 2 || std::strcmp(argv[1], "play") != 0) {
        usage();
        return 2;
    }

    Options o;
    o.key = defaultKeyPath();
    o.sock = "/tmp/pibridge.sock";

    static const struct option longopts[] = {
        {"host", required_argument, nullptr, 'H'},
        {"user", required_argument, nullptr, 'u'},
        {"key", required_argument, nullptr, 'k'},
        {"sock", required_argument, nullptr, 's'},
        {"device", required_argument, nullptr, 'd'},
        {"receiver", required_argument, nullptr, 'C'},
        {"mode", required_argument, nullptr, 'm'},
        {"rate", required_argument, nullptr, 'r'},
        {"buffer-ms", required_argument, nullptr, 'b'},
        {"target-ms", required_argument, nullptr, 't'},
        {"gain-db", required_argument, nullptr, 'g'},
        {"pace", required_argument, nullptr, 'c'},
        {"taps", required_argument, nullptr, 'T'},
        {"phases", required_argument, nullptr, 'P'},
        {"max-ppm", required_argument, nullptr, 'p'},
        {"block-ms", required_argument, nullptr, 'B'},
        {"steer", no_argument, nullptr, 'n'},
        {"recursive", no_argument, nullptr, 'R'},
        {"die-with-parent", no_argument, nullptr, 'D'},
        {"no-steer", no_argument, nullptr, 'N'},
        {"local-out", required_argument, nullptr, 'l'},
        {"metrics", required_argument, nullptr, 'M'},
        {"verbose", no_argument, nullptr, 'v'},
        {"status-json", no_argument, nullptr, 'J'},
        {"help", no_argument, nullptr, '?'},
        {nullptr, 0, nullptr, 0},
    };

    optind = 2;
    int c;
    while ((c = getopt_long(argc, argv, "v", longopts, nullptr)) != -1) {
        switch (c) {
            case 'H': o.host = optarg; break;
            case 'u': o.user = optarg; break;
            case 'k': o.key = optarg; break;
            case 's': o.sock = optarg; break;
            case 'd': o.device = optarg; break;
            case 'C': o.receiver = optarg; break;
            case 'm': o.mode = optarg; break;
            case 'r': o.outRate = std::atof(optarg); break;
            case 'b': o.bufferMs = std::atof(optarg); break;
            case 't': o.targetMs = std::atof(optarg); break;
            case 'g': o.gainDb = std::atof(optarg); break;
            case 'c': o.pace = optarg; o.paceExplicit = true; break;
            case 'T': o.taps = std::atoi(optarg); break;
            case 'P': o.phases = std::atoi(optarg); break;
            case 'p': o.maxPpm = std::atof(optarg); break;
            case 'B': o.blockMs = std::atof(optarg); break;
            case 'n': o.steer = true; break;
            case 'R': o.recursive = true; break;
            case 'D': o.dieWithParent = true; break;
            case 'N': o.steer = false; break;
            case 'l': o.localOut = optarg; break;
            case 'M': o.metricsPath = optarg; break;
            case 'v': o.verbose = true; break;
            case 'J': o.statusJson = true; break;
            default: usage(); return 2;
        }
    }
    for (; optind < argc; ++optind) o.inputs.push_back(argv[optind]);
    if (o.inputs.empty()) { usage(); return 2; }
    o.file = o.inputs.front();   // display name for the header line
    if (o.mode != "pcm" && o.mode != "dsd") {
        std::fprintf(stderr, "pibridge: --mode must be pcm or dsd\n");
        return 2;
    }

    // --- playlist --------------------------------------------------------
    std::string err;
    std::vector<std::string> tracks = collectTracks(o.inputs, o.recursive, err);
    if (!err.empty()) std::fprintf(stderr, "pibridge: %s\n", err.c_str());
    if (tracks.empty()) {
        std::fprintf(stderr, "pibridge: no playable audio found\n");
        return 1;
    }
    if (tracks.size() > 1) {
        std::fprintf(stderr, "pibridge: playlist of %zu tracks\n", tracks.size());
    }

    // Announce the full queue before playback, so a UI can list every song and
    // let the listener pick one instead of waiting for each @track to arrive.
    {
        const bool many = tracks.size() > 1;
        if (many || o.statusJson) {
            const std::size_t limit = 2000;   // guard against a pathological folder
            for (std::size_t i = 0; i < tracks.size() && i < limit; ++i) {
                std::string esc;
                esc.reserve(tracks[i].size() + 8);
                for (char c : tracks[i]) {
                    if (c == '"' || c == '\\') esc.push_back('\\');
                    if (c != '\n' && c != '\r') esc.push_back(c);
                }
                std::fprintf(stderr, "@item {\"index\":%zu,\"total\":%zu,\"path\":\"%s\"}\n",
                             i + 1, tracks.size(), esc.c_str());
            }
            if (tracks.size() > limit) {
                std::fprintf(stderr, "pibridge: queue truncated to %zu for display\n", limit);
            }
        }
    }

    // --- peek the first decodable track for its rate ---------------------
    // A folder can contain files CoreAudio cannot open (sample libraries are
    // full of them). One bad file must not abort the whole queue, so walk
    // forward until something opens.
    std::string wavPath;
    double inRate = 0.0;
    {
        std::size_t good = tracks.size();
        long skipped = 0;
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            std::string e;
            std::string candidate;
            const std::atomic<bool>* probeDone = nullptr;
            if (!materialise(tracks[i], candidate, e, &probeDone)) { ++skipped; continue; }
            if (probeDone && !waitForHeader(candidate, *probeDone, e)) { ++skipped; continue; }
            WavReader peek;
            std::string e2;
            if (!peek.open(candidate, e2)) continue;
            tracks.erase(tracks.begin(), tracks.begin() + static_cast<long>(i));
            wavPath = candidate;
            inRate = peek.rate();
            good = i;
            break;
        }
        if (skipped > 0) {
            std::fprintf(stderr, "pibridge: skipped %ld undecodable file%s while looking for the first track\n",
                         skipped, skipped == 1 ? "" : "s");
        }
        if (inRate == 0.0) {
            std::fprintf(stderr, "pibridge: no decodable audio in the selection\n");
            return 1;
        }
    }

    // --- pick the output rate -------------------------------------------
    // Choose the DAC rate that is an *exact* integer multiple of the file's.
    // Classifying by "is it a multiple of 48000" sent 32/64/128 kHz files to
    // 352800 Hz at 11.025x / 5.5125x / 2.7563x, where 384000 Hz would have been
    // an exact 12x / 6x / 3x. That is more interpolation error and a less clean
    // clock relationship for no benefit.
    if (o.outRate <= 0.0) {
        const double k384 = 384000.0 / inRate;
        const double k352 = 352800.0 / inRate;
        const bool exact384 = std::fabs(k384 - std::round(k384)) < 1e-9;
        const bool exact352 = std::fabs(k352 - std::round(k352)) < 1e-9;
        if (exact384) {
            o.outRate = 384000.0;  // K11 maximum, and an exact ratio
        } else if (exact352) {
            o.outRate = 352800.0;  // K11's maximum for the 44.1 family
        } else {
            // Neither is exact (unusual rate): fall back to the clock family the
            // rate most plausibly belongs to, so the ratio is at least close.
            o.outRate = (static_cast<long>(inRate) % 48000) == 0 ? 384000.0 : 352800.0;
        }
    }
    double bufferMs = o.bufferMs;
    double targetMs = o.targetMs;
    if (targetMs >= bufferMs) targetMs = bufferMs * 0.5;

    // The clock pacer was written against aplay: it waits for aplay to fill up,
    // then drains it to a target. hqdac already holds an exact device buffer and
    // applies backpressure through its own ring, so pacing it again fights it —
    // measured, clock pacing left the device buffer at 5 ms against a 20 ms
    // request while backpressure held it at 20.08 ms.
    if (o.receiver != "aplay" && !o.paceExplicit) o.pace = "backpressure";

    double blockMs = o.blockMs;
    if (blockMs <= 0.0) {
        // A block equal to the buffer means every write waits for a full
        // drain; keep blocks at a fraction of the buffer.
        blockMs = std::min(10.0, bufferMs / 4.0);
    }
    if (blockMs < 1.0) blockMs = 1.0;
    if (blockMs > 100.0) blockMs = 100.0;
    const long blockFrames = std::max(1L, static_cast<long>(o.outRate * blockMs / 1000.0));
    const std::size_t bytesPerFrame = 8;                            // stereo, 32-bit
    // Live gain. The app writes "gain <dB>" lines to our stdin; a ramp per
    // block reaches the target smoothly instead of stepping (zipper noise).
    std::atomic<double> gainTarget{std::pow(10.0, o.gainDb / 20.0)};
    double gainNow = std::pow(10.0, o.gainDb / 20.0);

    Asrc asrc(inRate, o.outRate, o.taps, o.phases);
    Sdm sdmL, sdmR;

    std::fprintf(stderr,
                 "pibridge: %s%s -> %s %s %g Hz, buffer %.0f ms, target %.0f ms\n",
                 o.file.c_str(),
                 tracks.size() > 1 ? (" (+" + std::to_string(tracks.size() - 1) + " more)").c_str()
                                   : "",
                 o.device.c_str(), formatName(o).c_str(), o.outRate, bufferMs, targetMs);

    // --- bring up the link ----------------------------------------------
    Child data;
    Child tele;
    Telemetry tm;
    StderrWatch watch;
    bool haveTele = false;

    if (o.localOut.empty()) {
        if (!ensureMaster(o.sock, o.key, o.user, o.host, err)) {
            std::fprintf(stderr, "pibridge: %s\n", err.c_str());
            return 1;
        }

        // The K11 has one playback subdevice, so a leftover receiver from an
        // earlier run means this stream cannot open at all. Previously that
        // surfaced as "output closed" after 0.8 s with no sound. Ask first, and
        // give the previous holder a moment to let go.
        {
            const std::string q =
                "sed -n \"s/^state: *\\([A-Z]*\\).*/\\1/p\" "
                "/proc/asound/card0/pcm0p/sub0/status 2>/dev/null";
            auto dacState = [&]() -> std::string {
                Child c;
                std::string e;
                if (!spawnRemote(o.sock, o.key, o.user, o.host, q, false, true, false, c, e)) {
                    return std::string();
                }
                std::string out;
                char b[128];
                for (;;) {
                    const ssize_t n = ::read(c.out, b, sizeof b);
                    if (n <= 0) break;
                    out.append(b, static_cast<std::size_t>(n));
                }
                c.reap();
                while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
                return out;
            };

            std::string state = dacState();
            if (!state.empty() && state != "closed") {
                std::fprintf(stderr,
                             "pibridge: the DAC is already in use (state: %s) — waiting up to 6 s "
                             "for it to free\n",
                             state.c_str());
                for (int i = 0; i < 12; ++i) {
                    ::usleep(500000);
                    state = dacState();
                    if (state.empty() || state == "closed") break;
                }
                if (!state.empty() && state != "closed") {
                    std::fprintf(stderr,
                                 "pibridge: the DAC is still held by another player.\n"
                                 "pibridge: stop it (or wait for it), then play again. A stale "
                                 "receiver frees itself within 5 s of its sender going away.\n");
                    return 1;
                }
                std::fprintf(stderr, "pibridge: the DAC is free, starting\n");
            }
        }

        // Start from a clean telemetry file regardless of receiver: a stale one
        // from an earlier hqdac run would otherwise be graphed as if it belonged
        // to the stream now playing.
        {
            Child clr;
            std::string clrErr;
            if (spawnRemote(o.sock, o.key, o.user, o.host, "rm -f " + statsPathForPid(), false,
                            false, false, clr, clrErr)) {
                clr.reap();
            }
        }

        // A named receiver is checked before a stream is opened, so a missing
        // binary is one clear message instead of ssh exiting 127 mid-playback.
        // The path is left unquoted on purpose: ~ has to expand remotely.
        if (o.receiver != "aplay") {
            const std::string exe = o.receiver.substr(0, o.receiver.find(' '));
            const bool safe =
                !exe.empty() && exe.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                                      "abcdefghijklmnopqrstuvwxyz"
                                                      "0123456789._/~+-") == std::string::npos;
            if (!safe) {
                std::fprintf(stderr, "pibridge: --receiver path '%s' contains unsupported characters\n",
                             exe.c_str());
                return 2;
            }
            Child chk;
            std::string ckErr;
            bool present = true;   // if the probe itself fails, let the spawn report it
            if (spawnRemote(o.sock, o.key, o.user, o.host,
                            "test -x " + exe + " && echo found || echo missing", false, true,
                            false, chk, ckErr)) {
                std::string out;
                char b[256];
                for (;;) {
                    const ssize_t n = ::read(chk.out, b, sizeof b);
                    if (n <= 0) break;
                    out.append(b, static_cast<std::size_t>(n));
                }
                chk.reap();
                present = out.find("found") != std::string::npos;
            }
            if (!present) {
                std::fprintf(stderr,
                             "pibridge: receiver '%s' is not installed on the Pi.\n"
                             "pibridge: run pibridge/receiver/install.sh, or use --receiver=aplay.\n",
                             exe.c_str());
                return 1;
            }
        }
        const long bufUs = static_cast<long>(bufferMs * 1000.0);
        const long perUs = std::max(2000L, bufUs / 8);
        char cmd[1024];
        if (o.receiver == "aplay") {
            std::snprintf(cmd, sizeof cmd,
                          "aplay -D %s -t raw -f %s -r %ld -c 2 --buffer-time=%ld --period-time=%ld -",
                          o.device.c_str(), formatName(o).c_str(), static_cast<long>(o.outRate),
                          bufUs, perUs);
        } else {
            // hqdac drives /dev/snd itself, so it also needs the userspace ring
            // depth: buffer-time is the kernel/DMA buffer, target-time is the
            // jitter budget on top of it.
            std::snprintf(cmd, sizeof cmd,
                          "%s -D %s -t raw -f %s -r %ld -c 2 --buffer-time=%ld --period-time=%ld "
                          "--target-time=%ld --stats-file=%s -",
                          o.receiver.c_str(), o.device.c_str(), formatName(o).c_str(),
                          static_cast<long>(o.outRate), bufUs, perUs,
                          static_cast<long>(targetMs * 1000.0), statsPathForPid().c_str());
        }
        if (!spawnRemote(o.sock, o.key, o.user, o.host, cmd, true, false, true, data, err)) {
            std::fprintf(stderr, "pibridge: %s\n", err.c_str());
            return 1;
        }
        if (data.err >= 0) {
            std::thread(stderrThread, data.err, &watch).detach();
        }
        if (spawnTelemetry(o, tele, err)) {
            haveTele = true;
            std::thread(telemetryThread, tele, &tm).detach();
        } else {
            std::fprintf(stderr, "pibridge: telemetry unavailable (%s); running open-loop\n",
                         err.c_str());
        }
    }

    std::atomic<bool> quitRequested{false};
    std::atomic<int> skipRequest{0};   // +1 next, -1 previous
    std::atomic<int> jumpRequest{-1};   // 0-based track index, -1 = none
    if (o.dieWithParent) {
        std::thread(parentWatchdog, ::getppid(), &quitRequested).detach();
    }
    std::thread(controlThread, &gainTarget, &quitRequested, &skipRequest, &jumpRequest,
                o.dieWithParent).detach();

    int outFd = -1;
    if (!o.localOut.empty()) {
        outFd = (o.localOut == "-") ? 1 : ::open(o.localOut.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (outFd < 0) {
            std::fprintf(stderr, "pibridge: cannot open %s\n", o.localOut.c_str());
            return 1;
        }
    } else {
        outFd = data.in;
    }

    std::FILE* msg = (o.localOut == "-") ? stderr : stdout;
    std::fprintf(msg, "pibridge: streaming %s, %s, %.0f Hz, tap %d\n", o.file.c_str(),
                 formatName(o).c_str(), o.outRate, o.taps);

    // --- the loop --------------------------------------------------------
    Stats st;
    std::vector<unsigned char> obuf;
    obuf.reserve(static_cast<std::size_t>(blockFrames * bytesPerFrame * 2));

    long lastFillWindow = 0;
    double trim = 1.0;          // 1.0 == the DAC's nominal clock
    double integral = 0.0;
    double lastSteer = nowSeconds();
    long long framesOut = 0;
    long long framesScheduled = 0;
    double t0 = nowSeconds();
    long long framesBase = 0;   // frames already banked when the schedule started
    const long fullFrames_ = static_cast<long>(o.outRate * bufferMs / 1000.0);
    const long targetFrames_ = (o.pace == "clock")
        ? static_cast<long>(o.outRate * targetMs / 1000.0)
        : static_cast<long>(fullFrames_ * 0.95);
    const bool clockPaced = (o.pace == "clock");

    // Steering can only move the buffer in clock mode; under backpressure the
    // ALSA buffer is full by construction and a trim would just shift pitch.
    const bool steering = haveTele && o.steer;
    bool ended = false;
    bool eof = false;
    double prefillLeft = targetMs / 1000.0;

    // Prefill: prime the Pi's buffer to the target depth before pacing starts.
    const long prefillFrames = static_cast<long>(o.outRate * prefillLeft);

    // Producers append here; the block writer drains it.
    std::vector<float> pcmL, pcmR;

    double peakLin = 0.0, sumSq = 0.0;
    long long levelFrames = 0;

    auto emit = [&](const float* L, const float* R, long n) {
        // Ramp across the block toward the requested gain.
        const double gTarget = gainTarget.load(std::memory_order_relaxed);
        const double gStep = (gTarget - gainNow) / static_cast<double>(n > 0 ? n : 1);
        // Levels are measured AFTER the fader, so the meters show what is
        // actually leaving for the DAC. Measured pre-gain the meters ignored the
        // volume control entirely and the knob felt disconnected from the app.
        // gainNow advances identically here and in the render loop below.
        double gScan = gainNow;
        for (long i = 0; i < n; ++i) {
            gScan += gStep;
            const double la = std::max(-1.0, std::min(1.0, static_cast<double>(L[i]) * gScan));
            const double ra = std::max(-1.0, std::min(1.0, static_cast<double>(R[i]) * gScan));
            const double a = std::fabs(la);
            const double b = std::fabs(ra);
            const double m = a > b ? a : b;
            if (m > peakLin) peakLin = m;
            sumSq += la * la + ra * ra;
        }
        levelFrames += 2LL * n;
        if (o.mode == "pcm") {
            const std::size_t base = obuf.size();
            obuf.resize(base + static_cast<std::size_t>(n) * bytesPerFrame);
            unsigned char* p = obuf.data() + base;
            for (long i = 0; i < n; ++i) {
                gainNow += gStep;
                const double a = std::max(-1.0, std::min(1.0, static_cast<double>(L[i]) * gainNow));
                const double b = std::max(-1.0, std::min(1.0, static_cast<double>(R[i]) * gainNow));
                const std::int32_t ia = static_cast<std::int32_t>(a * 2147483520.0);
                const std::int32_t ib = static_cast<std::int32_t>(b * 2147483520.0);
                std::memcpy(p + i * 8, &ia, 4);
                std::memcpy(p + i * 8 + 4, &ib, 4);
            }
        } else {
            const std::size_t base = obuf.size();
            obuf.resize(base + static_cast<std::size_t>(n) * bytesPerFrame);
            unsigned char* p = obuf.data() + base;
            for (long i = 0; i < n; ++i) {
                gainNow += gStep;
                const std::uint32_t wl = sdmL.word32(static_cast<double>(L[i]) * gainNow);
                const std::uint32_t wr = sdmR.word32(static_cast<double>(R[i]) * gainNow);
                // DSD_U32_BE: oldest sample bit in the MSB, so store big-endian.
                p[i * 8 + 0] = static_cast<unsigned char>(wl >> 24);
                p[i * 8 + 1] = static_cast<unsigned char>(wl >> 16);
                p[i * 8 + 2] = static_cast<unsigned char>(wl >> 8);
                p[i * 8 + 3] = static_cast<unsigned char>(wl);
                p[i * 8 + 4] = static_cast<unsigned char>(wr >> 24);
                p[i * 8 + 5] = static_cast<unsigned char>(wr >> 16);
                p[i * 8 + 6] = static_cast<unsigned char>(wr >> 8);
                p[i * 8 + 7] = static_cast<unsigned char>(wr);
            }
        }
    };

    // In clock mode the write schedule is "stay `target` frames ahead of the
    // consumer": frame k is due at t0 + (k - framesBase) / (rate * trim).
    auto flushBlock = [&](bool paced) {
        if (obuf.empty()) return;
        framesScheduled += static_cast<long long>(obuf.size() / bytesPerFrame);
        if (!writeAll(outFd, obuf.data(), obuf.size())) {
            // The receiver exits for one common, non-network reason: the DAC is
            // already held by another player. It says so on stderr, but that
            // stream is only scanned for "underrun", so the reason was being
            // dropped and the user got "the Pi link dropped?" — which sent them
            // looking at the network instead of at a stranded process.
            std::string why;
            {
                std::lock_guard<std::mutex> lock(watch.mu);
                std::string t = watch.text;
                for (char& c : t) if (c == '\r') c = '\n';
                std::size_t at = 0;
                while (at < t.size()) {
                    std::size_t nl = t.find('\n', at);
                    if (nl == std::string::npos) nl = t.size();
                    std::string line = t.substr(at, nl - at);
                    if (line.find("busy") != std::string::npos ||
                        line.find("failed") != std::string::npos ||
                        line.find("cannot") != std::string::npos ||
                        line.find("no input") != std::string::npos) {
                        why = line;
                    }
                    at = nl + 1;
                }
            }
            if (!why.empty()) {
                std::fprintf(stderr, "pibridge: the receiver stopped: %s\n", why.c_str());
            } else {
                std::fprintf(stderr, "pibridge: output closed (the Pi link dropped?)\n");
            }
            ended = true;
        }
        obuf.clear();
        if (paced) {
            const double due =
                t0 + static_cast<double>(framesScheduled - framesBase) / (o.outRate * trim);
            sleepUntil(due);
        }
    };

    // Block buffers for the ASRC output.
    pcmL.reserve(static_cast<std::size_t>(blockFrames));
    pcmR.reserve(static_cast<std::size_t>(blockFrames));

    long long pending = 0;  // output frames waiting for a full block
    long long prefillDone = 0;
    // 0 PREFILL (aplay fills its own buffer), 1 SETTLE (let it drain to the
    // target so the controller has headroom both ways), 2 RUN (paced).
    int stage = (clockPaced && o.localOut.empty()) ? 0 : 2;
    const long fullFrames = static_cast<long>(o.outRate * bufferMs / 1000.0);
    const long settleFrames = targetFrames_;
    const double settleDeadline = nowSeconds() + 2.0;
    double lastReport = nowSeconds();
    long lastDelay = -1;

    // Playlist state. The DAC rate and the SSH link stay up across tracks, so
    // consecutive tracks join without renegotiating ALSA or reconnecting.
    long trackIndex = 0;
    std::string asrcRateKey;
    std::string jsonName;

    for (std::size_t ti = 0; ti < tracks.size() && !ended && !g_stop &&
                             !quitRequested.load(std::memory_order_relaxed); ++ti) {
        std::string tErr;
        std::string tw;
        const std::atomic<bool>* decDone = nullptr;
        if (!materialise(tracks[ti], tw, tErr, &decDone)) {
            std::fprintf(stderr, "pibridge: skipping %s: %s\n", tracks[ti].c_str(), tErr.c_str());
            continue;
        }
        if (decDone && !waitForHeader(tw, *decDone, tErr)) {
            std::fprintf(stderr, "pibridge: skipping %s: %s\n", tracks[ti].c_str(), tErr.c_str());
            continue;
        }
        WavReader rd;
        if (!rd.open(tw, tErr)) {
            std::fprintf(stderr, "pibridge: skipping %s: %s\n", tracks[ti].c_str(), tErr.c_str());
            continue;
        }
        // The decoder is usually still running. Read the file as it grows so
        // playback starts on the first seconds rather than at end-of-transcode.
        if (decDone && !waitForAudio(tw, rd.rate(), rd.channels(), rd.frameBytes(), 0.5, *decDone,
                                     tErr)) {
            // Even a partial file can play; only give up if nothing arrived.
            if (!decDone->load(std::memory_order_relaxed) &&
                std::remove(tw.c_str()) == 0) {
                continue;
            }
        }
        if (decDone) rd.setGrowing(decDone);
        const double trackRate = rd.rate();
        const std::string rateKey = std::to_string(static_cast<long>(trackRate));
        if (rateKey != asrcRateKey) {
            // New source rate: rebuild the converter and reset the modulators so
            // the new stream starts from a clean state.
            asrc = Asrc(trackRate, o.outRate, o.taps, o.phases);
            sdmL.reset();
            sdmR.reset();
            asrcRateKey = rateKey;
        }
        trackIndex = static_cast<long>(ti) + 1;
        eof = false;   // end-of-file is per track, not per session

        // The app reads this to draw the queue.
        jsonName.clear();
        for (char c : tracks[ti]) {
            if (c == '"' || c == '\\') jsonName.push_back('\\');
            if (c != '\n' && c != '\r') jsonName.push_back(c);
        }
        std::fprintf(stderr, "@track {\"index\":%ld,\"total\":%zu,\"path\":\"%s\"}\n",
                     trackIndex, tracks.size(), jsonName.c_str());

        bool trackDone = false;
        while (!ended && !g_stop && !trackDone &&
               !quitRequested.load(std::memory_order_relaxed) &&
               skipRequest.load(std::memory_order_relaxed) == 0 &&
               jumpRequest.load(std::memory_order_relaxed) < 0) {
        // SETTLE: drain aplay's start-up fill down to the target before the
        // timeline starts. Writing through it instead leaves the buffer at
        // capacity, where a faster trim cannot raise it, so a slow drift can
        // only ever push it down into an underrun.
        if (stage == 1) {
            const bool drained = haveTele && tm.stateCode.load(std::memory_order_relaxed) == 1 &&
                                 tm.delay.load(std::memory_order_relaxed) <= settleFrames;
            if (drained || nowSeconds() > settleDeadline) {
                stage = 2;
                t0 = nowSeconds();
                framesBase = framesScheduled;
            } else {
                ::usleep(3000);
                continue;
            }
        }
        // Fill the ASRC from the file as needed and pull output.
        for (;;) {
            float l = 0.0f, r = 0.0f, a = 0.0f, b = 0.0f;
            if (!asrc.produce(a, b)) {
                if (eof) { trackDone = true; break; }
                if (!rd.read(l, r)) { eof = true; trackDone = true; break; }
                asrc.push(l, r);
                continue;
            }
            pcmL.push_back(a);
            pcmR.push_back(b);
            ++pending;
            if (pending >= blockFrames) break;
        }
        if (pcmL.empty()) break;   // track finished (or skipped)

        const long n = static_cast<long>(pcmL.size());
        emit(pcmL.data(), pcmR.data(), n);
        framesOut += n;
        pcmL.clear();
        pcmR.clear();
        pending = 0;

        bool paced = (stage == 2);
        if (clockPaced && o.localOut.empty()) {
            prefillDone += n;
            if (stage == 0) {
                // Write freely until aplay's buffer is full, then settle.
                const bool full = haveTele &&
                                  tm.stateCode.load(std::memory_order_relaxed) == 1 &&
                                  tm.delay.load(std::memory_order_relaxed) >= fullFrames * 9 / 10;
                if (full || prefillDone >= prefillFrames * 2) stage = 1;
                paced = false;
            }
        }
        flushBlock(paced && o.localOut.empty());

        // The receiver publishes a window every 250 ms; forward each one as it
        // appears. Emitting from the 1 Hz status block instead would keep one
        // window in four and frame the trace as if the buffer were quieter than
        // it is.
        if (o.statusJson) {
            const long seenWindows = tm.devSeq.load(std::memory_order_relaxed);
            if (seenWindows != lastFillWindow) {
                const long dMin = tm.devMin.load(std::memory_order_relaxed);
                const long dMax = tm.devMax.load(std::memory_order_relaxed);
                const long dMean = tm.devMean.load(std::memory_order_relaxed);
                const long dRate = tm.devRate.load(std::memory_order_relaxed);
                const long dBuf = tm.devBuffer.load(std::memory_order_relaxed);
                const long dRing = tm.devRing.load(std::memory_order_relaxed);
                const long dXr = tm.devXruns.load(std::memory_order_relaxed);
                if (dMin >= 0 && dRate > 0) {
                    lastFillWindow = seenWindows;
                    const double k = 1000.0 / static_cast<double>(dRate);
                    std::fprintf(stderr,
                                 "@fill {\"t\":%.2f,\"min_ms\":%.3f,\"mean_ms\":%.3f,"
                                 "\"max_ms\":%.3f,\"ring_ms\":%.3f,\"buffer_ms\":%.3f,"
                                 "\"p05_ms\":%.3f,\"p50_ms\":%.3f,\"p95_ms\":%.3f,"
                                 "\"dip_run\":%ld,\"dev_xruns\":%ld,\"windows\":%ld}\n",
                                 nowSeconds() - t0, static_cast<double>(dMin) * k,
                                 static_cast<double>(dMean) * k,
                                 static_cast<double>(dMax) * k,
                                 static_cast<double>(dRing) * k,
                                 static_cast<double>(dBuf) * k,
                                 static_cast<double>(tm.devP05.load(std::memory_order_relaxed)) * k,
                                 static_cast<double>(tm.devP50.load(std::memory_order_relaxed)) * k,
                                 static_cast<double>(tm.devP95.load(std::memory_order_relaxed)) * k,
                                 tm.devRun.load(std::memory_order_relaxed), dXr, seenWindows);
                }
            }
        }

        // --- closed loop ------------------------------------------------
        // In clock mode the ASRC ratio is the only actuator, and the Pi's
        // reported buffer depth is the only honest observable: it counts the
        // frames sitting unplayed in the DAC's buffer. Holding that depth at
        // `targetMs` is what keeps the pipe from draining or creeping.
        if (steering && stage >= 2) {
            const double now = nowSeconds();
            const double dt = now - lastSteer;
            if (dt > 0.05 && tm.stateCode.load(std::memory_order_relaxed) == 1) {
                lastSteer = now;
                const long d = tm.delay.load(std::memory_order_relaxed);
                const double err = static_cast<double>(targetFrames_ - d) / o.outRate;  // seconds
                integral += err * dt;
                if (integral > 0.20) integral = 0.20;
                if (integral < -0.20) integral = -0.20;

                const double Kp = 0.25;
                const double Ki = 0.004;
                double c = Kp * err + Ki * integral;
                const double cap = o.maxPpm * 1e-6;
                if (c > cap) c = cap;
                if (c < -cap) c = -cap;
                const double prev = trim;
                trim = 1.0 + c;
                // 0.5 ppm per update: a 100 ppm correction takes about 4 s,
                // which is under a tenth of a cent of pitch movement.
                const double slew = 0.5e-6;
                if (trim - prev > slew) trim = prev + slew;
                if (prev - trim > slew) trim = prev - slew;
                asrc.setTrim(trim);

                st.ppm.push_back((trim - 1.0) * 1e6);
                st.delayMs.push_back(static_cast<double>(d) * 1000.0 / o.outRate);
                if (d < st.minDelay) st.minDelay = d;
                if (d > st.maxDelay) st.maxDelay = d;
                if (d == 0) ++st.underruns;
                lastDelay = d;
            }
        } else if (haveTele) {
            const int sc = tm.stateCode.load(std::memory_order_relaxed);
            const long d = tm.delay.load(std::memory_order_relaxed);
            if (sc == 1) {
                st.delayMs.push_back(static_cast<double>(d) * 1000.0 / o.outRate);
                if (d < st.minDelay) st.minDelay = d;
                if (d > st.maxDelay) st.maxDelay = d;
                if (d == 0) ++st.underruns;
                lastDelay = d;
            }
        }

        if ((o.verbose || o.statusJson) && nowSeconds() - lastReport >= 1.0) {
            lastReport = nowSeconds();
            const double fillMs =
                lastDelay >= 0 ? static_cast<double>(lastDelay) * 1000.0 / o.outRate : -1.0;
            const double peakDb = peakLin > 0.0 ? 20.0 * std::log10(peakLin) : -120.0;
            const double rmsDb = levelFrames > 0 && sumSq > 0.0
                                     ? 10.0 * std::log10(sumSq / static_cast<double>(levelFrames))
                                     : -120.0;
            peakLin = 0.0;
            sumSq = 0.0;
            levelFrames = 0;
            if (o.statusJson) {
                // One JSON object per line, prefixed so it cannot be confused
                // with engine prose. The GUI parses exactly this.
                std::fprintf(stderr,
                             "@status {\"t\":%.2f,\"fill_ms\":%.2f,\"buffer_ms\":%.1f,"
                             "\"trim_ppm\":%.2f,\"underruns\":%ld,\"frames\":%lld,"
                             "\"peak_db\":%.1f,\"rms_db\":%.1f,\"xruns\":%ld}\n",
                             nowSeconds() - t0, fillMs, bufferMs, (trim - 1.0) * 1e6,
                             st.underruns, framesOut, peakDb, rmsDb,
                             watch.xruns.load(std::memory_order_relaxed));

            } else {
                std::fprintf(stderr, "  t=%5.1fs  fill=%6.1f/%.0f ms  trim=%+7.1f ppm\n",
                             nowSeconds() - t0, fillMs, bufferMs, (trim - 1.0) * 1e6);
            }
        }
        }  // per-track playback loop

        // A skip request consumes the flag and moves to the next (or previous)
        // track. Previous rewinds to the start of this track if we are deep in
        // the playlist, which is what a transport button should do.
        const int jump = jumpRequest.exchange(-1, std::memory_order_relaxed);
        if (jump >= 0) {
            if (static_cast<std::size_t>(jump) < tracks.size()) {
                // -1 because the for-loop increments.
                ti = static_cast<std::size_t>(jump) - 1;
            }
            skipRequest.store(0, std::memory_order_relaxed);
        } else {
            const int skip = skipRequest.exchange(0, std::memory_order_relaxed);
            if (skip < 0) {
                if (ti > 0) ti = static_cast<std::size_t>(static_cast<long>(ti) - 2);
            }
        }
    }  // playlist

    st.seconds = nowSeconds() - t0;

    // Drain: pad the tail so the DAC plays out the buffer instead of being cut.
    if (!ended && o.localOut.empty()) {
        std::vector<float> z(static_cast<std::size_t>(blockFrames), 0.0f);
        for (long i = 0; i < 3; ++i) {
            emit(z.data(), z.data(), blockFrames);
            writeAll(outFd, obuf.data(), obuf.size());
            obuf.clear();
        }
        ::usleep(static_cast<useconds_t>(bufferMs * 1000));
    }

    if (outFd > 1 && !o.localOut.empty()) ::close(outFd);
    if (!o.localOut.empty()) {
        std::fprintf(msg, "pibridge: wrote %lld frames to %s\n", framesOut, o.localOut.c_str());
    } else {
        data.closeIn();
        data.reap();
        if (haveTele) {
            ::close(tele.out);
            tele.reap();
        }
        std::string aplayErr;
        {
            std::lock_guard<std::mutex> lock(watch.mu);
            aplayErr = watch.text;
        }
        if (!aplayErr.empty()) {
            std::string cleaned;
            for (char ch : aplayErr) if (ch != '\r') cleaned.push_back(ch);
            // Trim to the useful diagnostic line(s).
            if (cleaned.size() > 400) cleaned = cleaned.substr(0, 400);
            std::fprintf(stderr, "pibridge: aplay said: %s\n", cleaned.c_str());
        }
    }
    // aplay's own xrun count is authoritative; the polled delay==0 test can
    // miss everything shorter than a poll interval.
    st.underruns += watch.xruns.load(std::memory_order_relaxed);

    // A stream that ended by itself having produced almost nothing is a
    // failure, not a finished song. Exit non-zero so the app says so.
    int exitCode = 0;
    if (ended && framesOut < static_cast<long long>(o.outRate * 0.05)) exitCode = 3;

    removeScratch();

    // --- report ---------------------------------------------------------
    const double dm = mean(st.delayMs);
    const double ds = stddev(st.delayMs);
    const double pm = mean(st.ppm);
    double pmin = 1e9, pmax = -1e9;
    for (double p : st.ppm) { if (p < pmin) pmin = p; if (p > pmax) pmax = p; }

    std::fprintf(msg, "pibridge: %lld output frames in %.2f s\n", framesOut, st.seconds);
    if (!st.delayMs.empty()) {
        std::fprintf(msg, "pibridge: buffer fill mean %.1f ms, sd %.2f ms, min %.1f, max %.1f\n", dm, ds,
                    static_cast<double>(st.minDelay) * 1000.0 / o.outRate,
                    static_cast<double>(st.maxDelay) * 1000.0 / o.outRate);
    }
    if (!st.ppm.empty()) {
        std::fprintf(msg, "pibridge: clock correction mean %+.1f ppm, range %+.1f..%+.1f\n", pm, pmin, pmax);
    }
    std::fprintf(msg, "pibridge: underruns %ld\n", st.underruns);

    if (!o.metricsPath.empty()) {
        std::FILE* f = std::fopen(o.metricsPath.c_str(), "w");
        if (f) {
            std::fprintf(f,
                         "{\"file\":\"%s\",\"mode\":\"%s\",\"in_rate\":%.0f,\"out_rate\":%.0f,"
                         "\"buffer_ms\":%.1f,\"target_ms\":%.1f,\"seconds\":%.3f,"
                         "\"fill_ms_mean\":%.3f,\"fill_ms_sd\":%.3f,\"fill_ms_min\":%.3f,"
                         "\"fill_ms_max\":%.3f,\"ppm_mean\":%.2f,\"ppm_min\":%.2f,"
                         "\"ppm_max\":%.2f,\"underruns\":%ld,\"samples\":%zu}\n",
                         o.file.c_str(), o.mode.c_str(), inRate, o.outRate, bufferMs, targetMs,
                         st.seconds, dm, ds,
                         st.delayMs.empty() ? 0.0 : static_cast<double>(st.minDelay) * 1000.0 / o.outRate,
                         st.delayMs.empty() ? 0.0 : static_cast<double>(st.maxDelay) * 1000.0 / o.outRate,
                         st.ppm.empty() ? 0.0 : pm, st.ppm.empty() ? 0.0 : pmin,
                         st.ppm.empty() ? 0.0 : pmax, st.underruns, st.delayMs.size());
            std::fclose(f);
        }
    }
    return exitCode;
}
