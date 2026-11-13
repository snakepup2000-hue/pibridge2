// hqdac — a static aarch64 playback receiver that drives /dev/snd directly.
//
// Why this exists: pibridge's README is honest that `aplay` is the floor. aplay
// fills the ALSA buffer before it starts, so fill depth is something you
// observe rather than control, and the drift controller has no authority
// against it. This is the "real protocol peer on the Pi" that README asks for:
// it issues the PCM ioctls itself, with no alsa-lib and no toolchain on the Pi.
//
// It is a drop-in for the aplay invocation pibridge already makes:
//
//   aplay -D hw:CARD=R2R,DEV=0 -t raw -f S32_LE -r 384000 -c 2 [buffer/period] -
//   hqdac -D hw:CARD=R2R,DEV=0 -t raw -f S32_LE -r 384000 -c 2 [buffer/period] -
//
// On stderr it prints the literal token "underrun!!!" per xrun, so pibridge's
// existing stderr watcher keeps counting correctly, plus optional telemetry.
//
// Build: see build-static.sh (gcc -static on arm64). No configure step.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <sound/asound.h>

// ---------------------------------------------------------------- utilities

static double nowSeconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static volatile sig_atomic_t g_stop = 0;
static int g_debug = 0;
static void onSignal(int sig) { (void)sig; g_stop = 1; }

// ---------------------------------------------------------------- arguments

struct FormatInfo {
    const char* name;
    snd_pcm_format_t id;
    unsigned bytesPerSample;
};

// The formats pibridge can emit. Anything else is rejected rather than guessed.
static const struct FormatInfo kFormats[] = {
    {"S16_LE", SNDRV_PCM_FORMAT_S16_LE, 2},
    {"S24_3LE", SNDRV_PCM_FORMAT_S24_3LE, 3},
    {"S32_LE", SNDRV_PCM_FORMAT_S32_LE, 4},
    {"DSD_U32_BE", SNDRV_PCM_FORMAT_DSD_U32_BE, 4},
};

static const struct FormatInfo* findFormat(const char* name) {
    for (size_t i = 0; i < sizeof kFormats / sizeof kFormats[0]; ++i) {
        if (strcasecmp(kFormats[i].name, name) == 0) return &kFormats[i];
    }
    return NULL;
}

struct Options {
    const char* device;
    const char* format;
    long rate;
    int channels;
    long bufferUs;
    long periodUs;
    long targetUs;
    int capsOnly;
    int stats;
    int drain;
    const char* statsFile;   // publish windowed DELAY stats here, for pibridge
    const char* teePath;     // also write every frame handed to the device here
};

static void usage(void) {
    fprintf(stderr,
            "usage: hqdac -D hw:CARD=R2R,DEV=0 [-t raw] -f S32_LE|S24_3LE|S16_LE|DSD_U32_BE\n"
            "             -r RATE -c CHANNELS [--buffer-time=US] [--period-time=US]\n"
            "             [--target-time=US] [--stats] [--stats-file PATH] [--tee PATH]\n"
            "             [--no-drain] [-]\n"
            "       hqdac -D hw:CARD=R2R,DEV=0 --caps\n");
}

// ---------------------------------------------------------------- /dev/snd

// card name -> number, so "-D hw:CARD=R2R,DEV=0" works like it does with aplay.
static int cardNumberByName(const char* name) {
    FILE* f = fopen("/proc/asound/cards", "r");
    if (!f) return -1;
    char line[512];
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        int num = -1;
        char id[64] = {0};
        if (sscanf(line, " %d [%63[^]]", &num, id) == 2) {
            char* end = id + strlen(id);
            while (end > id && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
            if (strcasecmp(id, name) == 0) { found = num; break; }
        }
    }
    fclose(f);
    return found;
}

// Accepts "hw:CARD=R2R,DEV=0", "hw:CARD=R2R", "hw:0,0", "hw:0".
// plughw/default are refused: there is no conversion layer here, and silently
// pretending otherwise would hide a format or rate mismatch.
static int parseDevice(const char* spec, int* card, int* dev, char* err, size_t errlen) {
    *card = 0;
    *dev = 0;
    if (!spec || strncmp(spec, "hw:", 3) != 0) {
        snprintf(err, errlen,
                 "unsupported device '%s': only hw:CARD=NAME[,DEV=n] and hw:card[,dev] are handled "
                 "(this receiver has no plug layer)",
                 spec ? spec : "");
        return -1;
    }
    const char* p = spec + 3;
    const char* nameTag = strstr(spec, "CARD=");
    if (nameTag) {
        char name[64] = {0};
        sscanf(nameTag + 5, "%63[^,]", name);
        const int n = cardNumberByName(name);
        if (n < 0) {
            snprintf(err, errlen, "no sound card named '%s' in /proc/asound/cards", name);
            return -1;
        }
        *card = n;
    } else if (sscanf(p, "%d", card) != 1) {
        snprintf(err, errlen, "cannot parse card number from '%s'", spec);
        return -1;
    }
    const char* devTag = strstr(spec, "DEV=");
    if (devTag) {
        if (sscanf(devTag + 4, "%d", dev) != 1) {
            snprintf(err, errlen, "cannot parse DEV from '%s'", spec);
            return -1;
        }
    } else if (!nameTag) {
        const char* comma = strchr(p, ',');
        if (comma) *dev = atoi(comma + 1);
    }
    return 0;
}

// ------------------------------------------------- hw_params construction

static struct snd_mask* maskOf(struct snd_pcm_hw_params* p, int param) {
    return &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

static struct snd_interval* intervalOf(struct snd_pcm_hw_params* p, int param) {
    return &p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

// Mirrors alsa-lib's snd_pcm_hw_params_any: all-ones masks, full intervals,
// rmask marking every param as refinable, then HW_REFINE lets the driver
// replace them with what it really supports. rmask is not optional — with it
// clear the kernel narrows nothing and the ranges come back as 0..UINT_MAX.
static void paramsAny(struct snd_pcm_hw_params* p) {
    memset(p, 0, sizeof *p);
    for (int i = SNDRV_PCM_HW_PARAM_FIRST_MASK; i <= SNDRV_PCM_HW_PARAM_LAST_MASK; ++i) {
        memset(maskOf(p, i), 0xff, sizeof(struct snd_mask));
    }
    for (int i = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; ++i) {
        struct snd_interval* v = intervalOf(p, i);
        v->min = 0;
        v->max = UINT_MAX;
        v->integer = 0;  // "any" is not integer-constrained; exact sets are
    }
    p->rmask = ~0u;
}

static void maskSingle(struct snd_pcm_hw_params* p, int param, unsigned bit) {
    struct snd_mask* m = maskOf(p, param);
    memset(m, 0, sizeof *m);
    m->bits[bit / 32] = (uint32_t)1u << (bit % 32);
}

static void intervalSingle(struct snd_pcm_hw_params* p, int param, unsigned v) {
    struct snd_interval* i = intervalOf(p, param);
    i->min = v;
    i->max = v;
    i->integer = 1;
    i->empty = 0;
    i->openmin = 0;
    i->openmax = 0;
}

static void intervalUpTo(struct snd_pcm_hw_params* p, int param, unsigned max) {
    struct snd_interval* i = intervalOf(p, param);
    i->min = 0;
    i->max = max;
    i->integer = 1;
    i->empty = 0;
    i->openmin = 0;
    i->openmax = 0;
}

static unsigned intervalMin(struct snd_pcm_hw_params* p, int param) {
    return intervalOf(p, param)->min;
}

static unsigned intervalMax(struct snd_pcm_hw_params* p, int param) {
    return intervalOf(p, param)->max;
}

static const char* formatNameById(unsigned id) {
    for (size_t i = 0; i < sizeof kFormats / sizeof kFormats[0]; ++i) {
        if ((unsigned)kFormats[i].id == id) return kFormats[i].name;
    }
    static char buf[32];
    snprintf(buf, sizeof buf, "0x%x", id);
    return buf;
}

static int printCaps(int fd) {
    struct snd_pcm_hw_params p;
    paramsAny(&p);
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &p) < 0) {
        fprintf(stderr, "hqdac: HW_REFINE: %s\n", strerror(errno));
        return 1;
    }
    printf("FORMAT:");
    for (unsigned f = 0; f <= SNDRV_PCM_FORMAT_LAST; ++f) {
        if (maskOf(&p, SNDRV_PCM_HW_PARAM_FORMAT)->bits[f / 32] & ((uint32_t)1u << (f % 32))) {
            printf(" %s", formatNameById(f));
        }
    }
    printf("\n");
    printf("RATE: %u..%u\n", intervalMin(&p, SNDRV_PCM_HW_PARAM_RATE),
           intervalMax(&p, SNDRV_PCM_HW_PARAM_RATE));
    printf("CHANNELS: %u..%u\n", intervalMin(&p, SNDRV_PCM_HW_PARAM_CHANNELS),
           intervalMax(&p, SNDRV_PCM_HW_PARAM_CHANNELS));
    printf("PERIOD_SIZE: %u..%u\n", intervalMin(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE),
           intervalMax(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE));
    printf("PERIOD_TIME: %u..%u\n", intervalMin(&p, SNDRV_PCM_HW_PARAM_PERIOD_TIME),
           intervalMax(&p, SNDRV_PCM_HW_PARAM_PERIOD_TIME));
    printf("BUFFER_SIZE: %u..%u\n", intervalMin(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE),
           intervalMax(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE));
    printf("BUFFER_TIME: %u..%u\n", intervalMin(&p, SNDRV_PCM_HW_PARAM_BUFFER_TIME),
           intervalMax(&p, SNDRV_PCM_HW_PARAM_BUFFER_TIME));
    return 0;
}

// ---------------------------------------------------------------- ring buffer

// A fixed-depth userspace ring decouples SSH jitter from the device buffer.
// Its fill is known exactly, so total in-flight latency is measurable rather
// than guessed: ring frames + device delay.
struct Ring {
    unsigned char* buf;
    size_t capFrames;
    size_t frameBytes;
    size_t readPos;   // frames
    size_t writePos;  // frames
    size_t avail;     // frames
    int eof;
    pthread_mutex_t mu;
};

static int ringInit(struct Ring* r, size_t capFrames, size_t frameBytes) {
    memset(r, 0, sizeof *r);
    r->capFrames = capFrames;
    r->frameBytes = frameBytes;
    r->buf = calloc(capFrames, frameBytes);
    if (!r->buf) return -1;
    pthread_mutex_init(&r->mu, NULL);
    return 0;
}

static void ringFree(struct Ring* r) {
    free(r->buf);
    pthread_mutex_destroy(&r->mu);
}

// Caller holds the lock.
static void ringPeekLocked(struct Ring* r, unsigned char* dst, size_t n) {
    const size_t first = (r->readPos + n <= r->capFrames) ? n : r->capFrames - r->readPos;
    memcpy(dst, r->buf + r->readPos * r->frameBytes, first * r->frameBytes);
    if (first < n) memcpy(dst + first * r->frameBytes, r->buf, (n - first) * r->frameBytes);
}

// Caller holds the lock.
static void ringPopLocked(struct Ring* r, size_t n) {
    r->readPos = (r->readPos + n) % r->capFrames;
    r->avail -= n;
}

// Caller holds the lock.
static void ringPushLocked(struct Ring* r, const unsigned char* src, size_t n) {
    const size_t first = (r->writePos + n <= r->capFrames) ? n : r->capFrames - r->writePos;
    memcpy(r->buf + r->writePos * r->frameBytes, src, first * r->frameBytes);
    if (first < n) memcpy(r->buf, src + first * r->frameBytes, (n - first) * r->frameBytes);
    r->writePos = (r->writePos + n) % r->capFrames;
    r->avail += n;
}

struct ProducerArg {
    struct Ring* ring;
    int fdIn;
    long* firstDataAtMicros;  // set once, for the start timeout
    int* abort;               // main thread sets this to release a blocked reader
    // When the last byte actually arrived. If pibridge is SIGKILLed the ssh
    // channel dies without ever sending EOF, so a blocking read() simply waits
    // forever and the DAC stays claimed. The main loop watches this instead.
    double* lastReadAt;
};

static double monoMicros(void) { return nowSeconds() * 1e6; }

static void* producerThread(void* arg) {
    struct ProducerArg* a = arg;
    struct Ring* r = a->ring;
    const size_t chunkFrames = 4096;
    const size_t chunkBytes = chunkFrames * r->frameBytes;
    unsigned char* scratch = malloc(chunkBytes + r->frameBytes);
    size_t carry = 0;  // bytes of an incomplete frame left over
    if (!scratch) {
        fprintf(stderr, "hqdac: out of memory in producer\n");
        pthread_mutex_lock(&r->mu);
        r->eof = 1;
        pthread_mutex_unlock(&r->mu);
        return NULL;
    }
    for (;;) {
        if (g_stop || (a->abort && *a->abort)) break;

        // Wait for room. Stalling here is what applies backpressure to the
        // sender, which is the whole point of running a bounded ring.
        pthread_mutex_lock(&r->mu);
        const size_t room = r->capFrames - r->avail;
        pthread_mutex_unlock(&r->mu);
        if (room == 0) { usleep(500); continue; }

        // poll() rather than a bare blocking read so the main thread can
        // always end this thread, even mid-stream on an error.
        struct pollfd pfd;
        pfd.fd = a->fdIn;
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int pr = poll(&pfd, 1, 200);
        if (pr == 0) continue;
        if (pr < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "hqdac: poll(stdin): %s\n", strerror(errno));
            break;
        }

        const size_t roomBytes = room * r->frameBytes;
        if (roomBytes <= carry) { usleep(200); continue; }
        size_t want = roomBytes - carry;
        if (want > chunkBytes) want = chunkBytes;
        if (want < r->frameBytes) { usleep(200); continue; }

        const ssize_t n = read(a->fdIn, scratch + carry, want);
        if (n == 0) break;  // EOF
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(200); continue; }
            fprintf(stderr, "hqdac: read(stdin): %s\n", strerror(errno));
            break;
        }
        if (a->firstDataAtMicros && *a->firstDataAtMicros < 0) {
            *a->firstDataAtMicros = (long)monoMicros();
        }
        if (a->lastReadAt) *a->lastReadAt = nowSeconds();
        const size_t total = carry + (size_t)n;
        const size_t whole = total / r->frameBytes;
        if (whole) {
            pthread_mutex_lock(&r->mu);
            const size_t take = whole < room ? whole : room;
            ringPushLocked(r, scratch, take);
            pthread_mutex_unlock(&r->mu);
            carry = total - take * r->frameBytes;
            if (carry) memmove(scratch, scratch + take * r->frameBytes, carry);
        } else {
            carry = total;
        }
    }
    free(scratch);
    pthread_mutex_lock(&r->mu);
    r->eof = 1;
    pthread_mutex_unlock(&r->mu);
    return NULL;
}

// ---------------------------------------------------------------- device ops

static long deviceDelay(int fd) {
    snd_pcm_sframes_t d = 0;
    if (ioctl(fd, SNDRV_PCM_IOCTL_DELAY, &d) < 0) return -1;
    return (long)d;
}

static long writeFrames(int fd, void* buf, size_t frames) {
    struct snd_xferi x;
    memset(&x, 0, sizeof x);
    x.buf = buf;
    x.frames = frames;
    if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) return -1;
    return (long)x.result;
}

struct HwConfig {
    unsigned bufferFrames;
    unsigned periodFrames;
    unsigned rate;
};

static int applyHwParams(int fd, const struct Options* o, const struct FormatInfo* fm,
                         struct HwConfig* out, char* err, size_t errlen) {
    struct snd_pcm_hw_params p;

    // Learn the device's real limits first: the K11 floors its period at
    // 125 us and its buffer at 41 us, so a request below that would otherwise
    // be rejected outright. Format, rate and channels are never clamped — those
    // are the ones a mismatch makes audible.
    paramsAny(&p);
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &p) < 0) {
        snprintf(err, errlen, "HW_REFINE: %s", strerror(errno));
        return -1;
    }
    const unsigned bufFloor = intervalMin(&p, SNDRV_PCM_HW_PARAM_BUFFER_TIME);
    const unsigned perFloor = intervalMin(&p, SNDRV_PCM_HW_PARAM_PERIOD_TIME);

    unsigned bufUs = (unsigned)o->bufferUs;
    unsigned perUs = (unsigned)o->periodUs;
    if (perUs < perFloor) perUs = perFloor;
    if (bufUs < bufFloor) bufUs = bufFloor;
    if (bufUs < perUs * 2) bufUs = perUs * 2;  // a buffer must hold two periods

    for (int attempt = 0; attempt < 2; ++attempt) {
        paramsAny(&p);
        if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &p) < 0) {
            snprintf(err, errlen, "HW_REFINE: %s", strerror(errno));
            return -1;
        }
        maskSingle(&p, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
        maskSingle(&p, SNDRV_PCM_HW_PARAM_FORMAT, (unsigned)fm->id);
        intervalSingle(&p, SNDRV_PCM_HW_PARAM_RATE, (unsigned)o->rate);
        intervalSingle(&p, SNDRV_PCM_HW_PARAM_CHANNELS, (unsigned)o->channels);
        if (attempt == 0) {
            intervalSingle(&p, SNDRV_PCM_HW_PARAM_BUFFER_TIME, bufUs);
            intervalSingle(&p, SNDRV_PCM_HW_PARAM_PERIOD_TIME, perUs);
        } else {
            // Bounded by the request rather than fully open: an unconstrained
            // retry lets the driver hand back its own default (128 ms here),
            // which is the opposite of what this program exists for.
            intervalUpTo(&p, SNDRV_PCM_HW_PARAM_BUFFER_TIME, bufUs);
            intervalUpTo(&p, SNDRV_PCM_HW_PARAM_PERIOD_TIME, perUs);
        }
        p.flags = SNDRV_PCM_HW_PARAMS_NORESAMPLE;  // the Mac already resampled
        p.rmask = ~0u;  // make the kernel re-derive period_size/buffer_size from *_time
        if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &p) == 0) {
            out->bufferFrames = intervalMin(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
            out->periodFrames = intervalMin(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
            out->rate = intervalMin(&p, SNDRV_PCM_HW_PARAM_RATE);
            if (!out->bufferFrames || !out->periodFrames) {
                snprintf(err, errlen, "driver returned an empty period/buffer size");
                return -1;
            }
            return 0;
        }
        if (attempt == 1) {
            snprintf(err, errlen,
                     "HW_PARAMS %s %ld Hz %d ch, buffer %u us, period %u us: %s", fm->name,
                     o->rate, o->channels, bufUs, perUs, strerror(errno));
            return -1;
        }
    }
    return -1;
}

static int applySwParams(int fd, const struct HwConfig* hw, char* err, size_t errlen) {
    struct snd_pcm_sw_params sw;
    memset(&sw, 0, sizeof sw);
    sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sw.period_step = 1;
    sw.avail_min = hw->periodFrames;      // wake poll() once a period is writable
    // Two buffers' worth means the kernel never auto-starts; this program owns
    // the start, and it starts only after the buffer has been filled.
    sw.start_threshold = (snd_pcm_uframes_t)hw->bufferFrames * 2;
    sw.stop_threshold = hw->bufferFrames;   // underrun reports XRUN, not a silent stop
    sw.silence_threshold = 0;
    sw.silence_size = 0;
    // The kernel wraps hw_ptr against this; it only has to be a large multiple
    // of the buffer size.
    sw.boundary = (snd_pcm_uframes_t)hw->bufferFrames * (0x7fffffffUL / hw->bufferFrames);
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
        snprintf(err, errlen, "SW_PARAMS: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int pcmPrepare(int fd) {
    return ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
}
static int pcmStart(int fd) {
    return ioctl(fd, SNDRV_PCM_IOCTL_START);
}
static int pcmDrain(int fd) {
    return ioctl(fd, SNDRV_PCM_IOCTL_DRAIN);
}
static int pcmDrop(int fd) {
    return ioctl(fd, SNDRV_PCM_IOCTL_DROP);
}

// ---------------------------------------------------------------- main loop

// Writes exactly the bytes handed to the device to a file, so the stream can be
// hashed and compared against the Mac's render. Diagnostic only.
static void teeWrite(int fd, const void* buf, size_t bytes) {
    if (fd < 0 || bytes == 0) return;
    const unsigned char* p = (const unsigned char*)buf;
    while (bytes > 0) {
        const ssize_t n = write(fd, p, bytes);
        if (n <= 0) {
            if (errno == EINTR) continue;
            static int warned = 0;
            if (!warned) {
                warned = 1;
                fprintf(stderr, "hqdac: tee write failed: %s\n", strerror(errno));
            }
            return;
        }
        p += n;
        bytes -= (size_t)n;
    }
}

static int runStream(const struct Options* o, const struct FormatInfo* fm, int fdDev,
                     const struct HwConfig* hw) {
    const size_t frameBytes = (size_t)fm->bytesPerSample * (size_t)o->channels;

    int teeFd = -1;
    if (o->teePath && *o->teePath) {
        teeFd = open(o->teePath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (teeFd < 0) {
            fprintf(stderr, "hqdac: cannot open tee file %s: %s\n", o->teePath,
                    strerror(errno));
            return 1;
        }
        fprintf(stderr, "hqdac: teeing device-bound frames to %s\n", o->teePath);
    }
    // Write the whole free space in one call rather than a fixed chunk: the
    // device buffer then spends its life near full instead of sawtoothing from
    // one period's worth of headroom down to almost nothing.
    const size_t chunkFrames = hw->bufferFrames;

    // The ring capacity is the latency budget, not a multiple of it: a sender
    // that writes at line rate will always keep it full, and its depth is what
    // the listener hears as delay. 4096 frames (~10.7 ms at 384 kHz) is the
    // floor, purely so the reader is not doing 200 tiny reads per millisecond.
    long targetFrames = (long)((double)o->targetUs * o->rate / 1e6);
    if (targetFrames < 1) targetFrames = 1;
    size_t capFrames = (size_t)targetFrames;
    if (capFrames < 4096) capFrames = 4096;

    struct Ring ring;
    if (ringInit(&ring, capFrames, frameBytes) != 0) {
        fprintf(stderr, "hqdac: ring alloc failed\n");
        return 1;
    }

    fprintf(stderr,
            "hqdac: %s -> %s %d ch, %u Hz, period %u frames, buffer %u frames (%.2f ms), "
            "prefill %.2f ms\n",
            fm->name, o->device, o->channels, hw->rate, hw->periodFrames, hw->bufferFrames,
            (double)hw->bufferFrames * 1000.0 / hw->rate,
            (double)targetFrames * 1000.0 / hw->rate);

    long firstDataAt = -1;
    int abortReader = 0;
    double lastReadAt = nowSeconds();
    struct ProducerArg pa = {&ring, 0, &firstDataAt, &abortReader, &lastReadAt};
    pthread_t producer;
    if (pthread_create(&producer, NULL, producerThread, &pa) != 0) {
        fprintf(stderr, "hqdac: cannot start reader thread\n");
        ringFree(&ring);
        return 1;
    }

    unsigned char* staging = malloc(chunkFrames * frameBytes);
    if (!staging) {
        fprintf(stderr, "hqdac: out of memory\n");
        ringFree(&ring);
        return 1;
    }

    int rc = 0;
    int started = 0;
    long xruns = 0;
    long long framesOut = 0;
    double lastStats = nowSeconds();
    double lastFill = 0;
    long winMin = LONG_MAX, winMax = 0, winSum = 0, winCount = 0;
    long lastDelayFrames = 0;
    // Percentiles, not extremes. DELAY occasionally reports a single low value
    // while the buffer is actually full (mean stayed 7.995 ms with sd 0.000
    // against an 8.000 ms buffer), so an absolute-minimum edge draws a pulse
    // that no listener would hear. A 256-bucket histogram gives p05/p50/p95
    // cheaply and lets a sustained dip be told apart from a lone bad read.
    int hist[256];
    long lowRun = 0, lowRunMax = 0;
    memset(hist, 0, sizeof hist);
    // Any reset of the window resets its histogram and dip runs too, or the
    // percentiles would describe a mix of two different periods.
#define WIN_RESET() do { \
        winMin = LONG_MAX; winMax = 0; winSum = 0; winCount = 0; \
        memset(hist, 0, sizeof hist); lowRun = 0; lowRunMax = 0; \
    } while (0)
    // Windows measured while the pipe is still filling describe the ramp, not
    // steady state: the buffer legitimately starts near zero and climbs. Stats
    // begin only once the device buffer has actually been primed once.
    int primed = 0;
    double primedAt = 0.0;
    // The fill ramp lasts up to a second: the device buffer climbs from empty
    // to full and the ring is still being fed. Those readings are real but they
    // describe the start-up, not the stream, so they are not published.
    const double settleSeconds = 1.0;
    // Counts published windows. Without it a reader cannot distinguish a fresh
    // window from the same file being re-read, and would graph one window many
    // times over.
    long winSeq = 0;
    const double startTimeout = 2.0;  // do not stall forever on a trickle of data

    for (;;) {
        if (g_stop) { pcmDrop(fdDev); break; }

        size_t avail;
        int eof;
        pthread_mutex_lock(&ring.mu);
        avail = ring.avail;
        eof = ring.eof;
        pthread_mutex_unlock(&ring.mu);

        // A sender that stops feeding mid-stream without closing stdin (pibridge
        // killed, ssh dropped) would otherwise hold the DAC forever. Five seconds
        // is far longer than any legitimate gap: the ring is a few ms deep and a
        // stalled producer drains it in that time.
        const double idleFor = nowSeconds() - lastReadAt;
        {
            const double limit = started ? 5.0 : 10.0;
            if (idleFor > limit && !eof) {
                fprintf(stderr,
                        "hqdac: no input for %.1f s — releasing the device\n", idleFor);
                pcmDrop(fdDev);
                rc = 1;
                break;
            }
        }

        if (!started) {
            if (eof && avail == 0) break;
            // Enough to fill the device buffer, or a trickle that has had long
            // enough and is never going to reach it.
            const size_t required =
                (size_t)targetFrames < hw->bufferFrames ? (size_t)targetFrames : hw->bufferFrames;
            const int ready = avail >= required ||
                              (avail > 0 && firstDataAt > 0 &&
                               monoMicros() - (double)firstDataAt > startTimeout * 1e6);
            if (!ready) { usleep(500); continue; }
            if (pcmPrepare(fdDev) < 0) {
                fprintf(stderr, "hqdac: PREPARE: %s\n", strerror(errno));
                rc = 1;
                break;
            }

            // A playback stream xruns the moment its buffer is empty, so
            // STARTing first is an immediate underrun (aplay avoids this by
            // filling first). Fill the device buffer, then start.
            int prefillFailed = 0;
            for (;;) {
                long d = deviceDelay(fdDev);
                if (d < 0) d = 0;
                long space = (long)hw->bufferFrames - d;
                if (space <= 0) break;
                size_t m = 0;
                pthread_mutex_lock(&ring.mu);
                m = ring.avail;
                if ((size_t)space < m) m = (size_t)space;
                if (m > chunkFrames) m = chunkFrames;
                // Peek only. Popping before the write meant a short write
                // silently discarded the frames the device did not take.
                if (m) ringPeekLocked(&ring, staging, m);
                pthread_mutex_unlock(&ring.mu);
                if (m == 0) break;
                const long w = writeFrames(fdDev, staging, m);
                if (w < 0) {
                    fprintf(stderr, "hqdac: WRITEI during prefill: %s\n", strerror(errno));
                    prefillFailed = 1;
                    break;
                }
                pthread_mutex_lock(&ring.mu);
                ringPopLocked(&ring, (size_t)w);
                pthread_mutex_unlock(&ring.mu);
                teeWrite(teeFd, staging, (size_t)w * frameBytes);
                framesOut += w;
            }
            if (prefillFailed) { rc = 1; break; }

            if (pcmStart(fdDev) < 0) {
                fprintf(stderr, "hqdac: START: %s\n", strerror(errno));
                rc = 1;
                break;
            }
            started = 1;
        }

        // Poll the device for how much is still queued there. This is the
        // number aplay never exposed: the kernel's delay in frames.
        //
        // A failed read is NOT an empty buffer. Treating -1 as zero made the
        // windowed minimum report dips to 0.003 ms that never happened, while
        // an independent /proc sampler over the same seconds never dropped
        // below 6.5 ms. Failures are held over and excluded from the stats.
        const long rawDelay = deviceDelay(fdDev);
        const int delayOk = rawDelay >= 0;
        if (g_debug && (!delayOk || rawDelay < (long)hw->bufferFrames / 4)) {
            fprintf(stderr, "hqdac: LOW raw=%ld ok=%d ring=%zu buf=%u t=%.3f\n", rawDelay,
                    delayOk, avail, hw->bufferFrames, nowSeconds());
        }
        long delay = delayOk ? rawDelay : lastDelayFrames;
        if (delay > (long)hw->bufferFrames) delay = (long)hw->bufferFrames;
        if (delayOk) lastDelayFrames = delay;
        lastFill = (double)delay * 1000.0 / (double)hw->rate;

        // Accumulate every DELAY reading into a window. A UI sampling one
        // value every 80 ms aliases against a 4-8 ms buffer and reports
        // phantom minimums; a windowed min/mean/max does not.
        if (delayOk && !primed && delay >= (long)hw->bufferFrames * 9 / 10) {
            // The ramp's low readings may already have landed in this window.
            // Throw them away at the moment the pipe primes, so the first
            // published window describes steady state only.
            primed = 1;
            primedAt = nowSeconds();
            WIN_RESET();
        }
        if (delayOk && !primed) {
            // ramp — excluded from the window
        } else if (delayOk) {
            if (delay < winMin) winMin = delay;
            if (delay > winMax) winMax = delay;
            winSum += delay;
            ++winCount;
            int b = (int)((double)delay * 256.0 / (double)(hw->bufferFrames + 1));
            if (b < 0) b = 0;
            if (b > 255) b = 255;
            ++hist[b];
            // A dip long enough to be heard lasts many polls; one bad read does
            // not. Track the longest consecutive run under half the buffer.
            if (delay < (long)hw->bufferFrames / 2) {
                if (++lowRun > lowRunMax) lowRunMax = lowRun;
            } else {
                lowRun = 0;
            }
        }

        if (nowSeconds() - lastStats > 0.25) {
            lastStats = nowSeconds();
            double ringMs = (double)avail * 1000.0 / (double)hw->rate;
            // Snapshot the window before it is reset, or the published values
            // would be the *next* window's empty state.
            // A window with no samples (the priming ramp) is not a reading of
            // zero — publishing it as one drew a dip to 0 ms at every stream
            // start that never happened. Publish nothing and keep the last
            // good window instead.
            if (winCount == 0 || nowSeconds() - primedAt < settleSeconds) {
                WIN_RESET();
                lastStats = nowSeconds();
                continue;
            }
            const long wMin = winMin;
            const long wMax = winMax;
            const long wRun = lowRunMax;
            const double wMean = (double)winSum / (double)winCount;

            // Percentiles from the histogram, in frames.
            const long total = winCount;
            long cum = 0;
            long p05 = wMin, p50 = wMin, p95 = wMax;
            int got5 = 0, got50 = 0, got95 = 0;
            for (int i = 0; i < 256 && (!got5 || !got50 || !got95); ++i) {
                if (!hist[i]) continue;
                cum += hist[i];
                // Bucket i covers frames [i*(B+1)/256, (i+1)*(B+1)/256); report
                // its midpoint so a single-bucket distribution reads as its
                // actual value instead of the bucket's floor.
                const long lo = (long)((double)i * (double)(hw->bufferFrames + 1) / 256.0);
                const long hi = (long)((double)(i + 1) * (double)(hw->bufferFrames + 1) / 256.0);
                const long value = (lo + hi) / 2;
                if (!got5 && cum >= total * 5 / 100) { p05 = value; got5 = 1; }
                if (!got50 && cum >= total * 50 / 100) { p50 = value; got50 = 1; }
                if (!got95 && cum >= total * 95 / 100) { p95 = value; got95 = 1; }
            }
            WIN_RESET();

            if (o->stats) {
                fprintf(stderr,
                        "hqdac: dev=%.2fms ring=%.2fms total=%.2fms xruns=%ld "
                        "(window p05=%.2f p50=%.2f p95=%.2f min=%.2f run=%ld)\n",
                        lastFill, ringMs, lastFill + ringMs, xruns,
                        (double)p05 * 1000.0 / hw->rate, (double)p50 * 1000.0 / hw->rate,
                        (double)p95 * 1000.0 / hw->rate, (double)wMin * 1000.0 / hw->rate,
                        wRun);
            }
            if (o->statsFile) {
                // One short line, overwritten in place: cheap for ssh to cat
                // and never grows without bound.
                const int f = open(o->statsFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (f >= 0) {
                    char line[256];
                    ++winSeq;
                    const int n = snprintf(line, sizeof line,
                                           "min %ld max %ld mean %ld xruns %ld rate %u "
                                           "buffer %u ring %ld frames %lld seq %ld "
                                           "p05 %ld p50 %ld p95 %ld run %ld\n",
                                           wMin, wMax, (long)wMean, xruns, hw->rate,
                                           hw->bufferFrames, (long)avail, framesOut, winSeq,
                                           p05, p50, p95, wRun);
                    if (n > 0) {
                        const ssize_t ig = write(f, line, (size_t)n);
                        (void)ig;
                    }
                    close(f);
                }
            }
        }

        long space = (long)hw->bufferFrames - delay;
        if (space <= 0) {
            if (eof && avail == 0) break;  // nothing left to feed it
            struct pollfd pfd = {fdDev, POLLOUT, 0};
            poll(&pfd, 1, 50);
            continue;
        }

        size_t n = 0;
        pthread_mutex_lock(&ring.mu);
        n = ring.avail;
        if ((size_t)space < n) n = (size_t)space;
        if (n > chunkFrames) n = chunkFrames;
        if (n) ringPeekLocked(&ring, staging, n);
        pthread_mutex_unlock(&ring.mu);

        if (n == 0) {
            if (eof) break;
            usleep(300);
            continue;
        }

        const long wrote = writeFrames(fdDev, staging, n);
        if (wrote >= 0) {
            // Consume exactly the frames the device accepted; anything it did
            // not take is still at the read position and is retried.
            pthread_mutex_lock(&ring.mu);
            ringPopLocked(&ring, (size_t)wrote);
            pthread_mutex_unlock(&ring.mu);
            teeWrite(teeFd, staging, (size_t)wrote * frameBytes);
        }
        if (wrote < 0) {
            if (errno == EPIPE) {
                ++xruns;
                fprintf(stderr, "underrun!!! (xrun %ld, dev was %.2fms)\n", xruns, lastFill);
                started = 0;  // re-prefill from the ring, then restart
                primed = 0;
                continue;
            }
            fprintf(stderr, "hqdac: WRITEI: %s\n", strerror(errno));
            rc = 1;
            break;
        }
        framesOut += wrote;
    }

    pthread_mutex_lock(&ring.mu);
    ring.eof = 1;
    pthread_mutex_unlock(&ring.mu);
    abortReader = 1;  // release the reader if it is still waiting on stdin
    pthread_join(producer, NULL);

    if (rc == 0 && o->drain && started && !g_stop) {
        if (pcmDrain(fdDev) < 0 && errno != ESTRPIPE) {
            fprintf(stderr, "hqdac: DRAIN: %s\n", strerror(errno));
        }
    }

    free(staging);
    ringFree(&ring);
    if (teeFd >= 0) {
        // fsync so a hash taken immediately after exit sees every byte.
        fsync(teeFd);
        close(teeFd);
    }
    fprintf(stderr, "hqdac: %lld frames out, %ld underruns\n", framesOut, xruns);
    return rc;
}

// ---------------------------------------------------------------- main

static int argValue(int argc, char** argv, int* i, const char* name, const char** out) {
    const size_t len = strlen(name);
    if (strncmp(argv[*i], name, len) == 0) {
        if (argv[*i][len] == '=') { *out = argv[*i] + len + 1; return 1; }
        if (argv[*i][len] == '\0') {
            if (*i + 1 < argc) { *out = argv[++*i]; return 1; }
            return -1;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    g_debug = (access("/tmp/hqdac.debug", F_OK) == 0);

    struct Options o;
    memset(&o, 0, sizeof o);
    o.device = "hw:0,0";
    o.format = "S32_LE";
    o.rate = 0;
    o.channels = 0;
    o.bufferUs = 8000;
    o.periodUs = 2000;
    o.targetUs = 15000;
    o.drain = 1;

    for (int i = 1; i < argc; ++i) {
        const char* v = NULL;
        int r;
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        if (!strcmp(argv[i], "-")) continue;  // stdin, explicit
        if (!strcmp(argv[i], "--caps") || !strcmp(argv[i], "--dump-hw-params")) {
            o.capsOnly = 1;
            continue;
        }
        if (!strcmp(argv[i], "--stats")) { o.stats = 1; continue; }
        if ((r = argValue(argc, argv, &i, "--tee", &v)) != 0) {
            if (r < 0) { usage(); return 2; }
            o.teePath = v;
            continue;
        }
        if ((r = argValue(argc, argv, &i, "--stats-file", &v)) != 0) {
            if (r < 0) { usage(); return 2; }
            o.statsFile = v;
            continue;
        }
        if (!strcmp(argv[i], "--no-drain")) { o.drain = 0; continue; }
        if ((r = argValue(argc, argv, &i, "-D", &v)) != 0) {
            if (r < 0) { usage(); return 2; }
            o.device = v;
            continue;
        }
        if ((r = argValue(argc, argv, &i, "--device", &v)) != 0) {
            if (r < 0) { usage(); return 2; }
            o.device = v;
            continue;
        }
        if ((r = argValue(argc, argv, &i, "-f", &v)) != 0) {
            if (r < 0) { usage(); return 2; }
            o.format = v;
            continue;
        }
        if ((r = argValue(argc, argv, &i, "-r", &v)) != 0) {
            if (r < 0) { usage(); return 2; }
            o.rate = atol(v);
            continue;
        }
        if ((r = argValue(argc, argv, &i, "-c", &v)) != 0) {
            if (r < 0) { usage(); return 2; }
            o.channels = atoi(v);
            continue;
        }
        if ((r = argValue(argc, argv, &i, "--buffer-time", &v)) != 0) {
            if (r < 0) { usage(); return 2; }
            o.bufferUs = atol(v);
            continue;
        }
        if ((r = argValue(argc, argv, &i, "--period-time", &v)) != 0) {
            if (r < 0) { usage(); return 2; }
            o.periodUs = atol(v);
            continue;
        }
        if ((r = argValue(argc, argv, &i, "--target-time", &v)) != 0) {
            if (r < 0) { usage(); return 2; }
            o.targetUs = atol(v);
            continue;
        }
        if ((r = argValue(argc, argv, &i, "-t", &v)) != 0) {
            if (r < 0) { usage(); return 2; }
            if (strcmp(v, "raw") != 0) {
                fprintf(stderr, "hqdac: only -t raw is supported (got '%s')\n", v);
                return 2;
            }
            continue;
        }
        fprintf(stderr, "hqdac: unknown argument '%s'\n", argv[i]);
        usage();
        return 2;
    }

    int card = 0, dev = 0;
    char err[512];
    if (parseDevice(o.device, &card, &dev, err, sizeof err) != 0) {
        fprintf(stderr, "hqdac: %s\n", err);
        return 2;
    }

    char path[64];
    snprintf(path, sizeof path, "/dev/snd/pcmC%dD%dp", card, dev);
    // Open nonblocking first: the K11 has one playback subdevice, and a
    // blocking open() with another player holding it just sleeps forever
    // instead of reporting the conflict. Clear O_NONBLOCK afterwards so
    // WRITEI blocks at the device's pace, which is the pacing we want.
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "hqdac: open %s: %s\n", path, strerror(errno));
        if (errno == EACCES) {
            fprintf(stderr, "hqdac: this user needs the 'audio' group for %s\n", path);
        } else if (errno == EBUSY) {
            fprintf(stderr, "hqdac: %s is busy (another player holds the DAC)\n", path);
        }
        return 1;
    }

    if (o.capsOnly) {
        const int rc = printCaps(fd);
        close(fd);
        return rc;
    }

    // Restore blocking I/O: WRITEI then waits for DMA space, which is the
    // device clock pacing the stream.
    {
        const int fl = fcntl(fd, F_GETFL);
        if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    }

    const struct FormatInfo* fm = findFormat(o.format);
    if (!fm) {
        fprintf(stderr, "hqdac: unsupported format '%s' (have S16_LE, S24_3LE, S32_LE, DSD_U32_BE)\n",
                o.format);
        close(fd);
        return 2;
    }
    if (o.rate <= 0 || o.channels <= 0) {
        fprintf(stderr, "hqdac: -r RATE and -c CHANNELS are required\n");
        close(fd);
        return 2;
    }
    if (o.bufferUs <= 0) o.bufferUs = 8000;
    if (o.periodUs <= 0) o.periodUs = 2000;
    if (o.targetUs <= 0) o.targetUs = 20000;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = onSignal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    struct HwConfig hw;
    if (applyHwParams(fd, &o, fm, &hw, err, sizeof err) != 0) {
        fprintf(stderr, "hqdac: %s\n", err);
        close(fd);
        return 1;
    }
    if (applySwParams(fd, &hw, err, sizeof err) != 0) {
        fprintf(stderr, "hqdac: %s\n", err);
        close(fd);
        return 1;
    }
    if (hw.rate != (unsigned)o.rate) {
        fprintf(stderr, "hqdac: warning: device chose %u Hz, asked for %ld\n", hw.rate, o.rate);
    }

    const int rc = runStream(&o, fm, fd, &hw);
    close(fd);
    return rc;
}
