// Minimal streaming WAV reader: PCM 16/24/32 and IEEE float32/64.
//
// Decoding of compressed and container formats is delegated to macOS's own
// afconvert (CoreAudio), which writes a float32 WAV; this reader is what the
// streaming loop consumes.

#pragma once

#include <unistd.h>

#include <atomic>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace pib {

class WavReader {
public:
    ~WavReader() { if (f_) std::fclose(f_); }

    // Growing mode: the file is still being written. A short read is not EOF,
    // it is "not yet"; wait and retry until *writerDone says the producer has
    // exited. This is what lets playback start ~0.2 s in instead of waiting for
    // a whole FLAC to be transcoded to WAV (3.4 s for a 7-minute track).
    void setGrowing(const std::atomic<bool>* writerDone) {
        growing_ = true;
        writerDone_ = writerDone;
    }

    bool open(const std::string& path, std::string& err) {
        f_ = std::fopen(path.c_str(), "rb");
        if (!f_) { err = "cannot open " + path; return false; }

        unsigned char riff[12];
        if (std::fread(riff, 1, 12, f_) != 12 || std::memcmp(riff, "RIFF", 4) != 0 ||
            std::memcmp(riff + 8, "WAVE", 4) != 0) {
            err = "not a RIFF/WAVE file: " + path;
            return false;
        }

        bool haveFmt = false;
        for (;;) {
            unsigned char hdr[8];
            if (std::fread(hdr, 1, 8, f_) != 8) { err = "truncated WAV (no data chunk)"; return false; }
            const std::uint32_t size = le32(hdr + 4);
            if (std::memcmp(hdr, "fmt ", 4) == 0) {
                std::vector<unsigned char> b(size);
                if (std::fread(b.data(), 1, size, f_) != size) { err = "truncated fmt chunk"; return false; }
                if (size < 16) { err = "short fmt chunk"; return false; }
                fmt_ = le16(b.data());
                ch_ = le16(b.data() + 2);
                rate_ = le32(b.data() + 4);
                bits_ = le16(b.data() + 14);
                if (fmt_ == 0xFFFE && size >= 40) fmt_ = le16(b.data() + 24);  // WAVE_FORMAT_EXTENSIBLE
                haveFmt = true;
            } else if (std::memcmp(hdr, "data", 4) == 0) {
                if (!haveFmt) { err = "data chunk before fmt"; return false; }
                dataStart_ = std::ftell(f_);
                dataBytes_ = size;
                dataRead_ = 0;
                break;
            } else {
                std::fseek(f_, static_cast<long>(size + (size & 1)), SEEK_CUR);
            }
        }

        bytesPerSample_ = static_cast<int>((bits_ + 7) / 8);
        frameBytes_ = static_cast<long>(bytesPerSample_ * ch_);
        if (ch_ < 1 || ch_ > 2) { err = "only mono/stereo supported"; return false; }
        if (bytesPerSample_ < 2 || bytesPerSample_ > 8) { err = "unsupported bit depth"; return false; }
        return true;
    }

    double rate() const { return static_cast<double>(rate_); }
    int channels() const { return ch_; }
    long frameBytes() const { return frameBytes_; }

    // Reads one frame of interleaved input, duplicated to stereo.
    bool read(float& l, float& r) {
        unsigned char buf[16];
        const long want = frameBytes_;
        // A growing file's declared data size is a placeholder while the writer
        // is running, so it is ignored; EOF comes from the writer instead.
        if (!growing_ && dataBytes_ && dataRead_ >= static_cast<long>(dataBytes_)) return false;
        for (;;) {
            const std::size_t got = std::fread(buf, 1, static_cast<std::size_t>(want), f_);
            if (got == static_cast<std::size_t>(want)) break;
            if (std::ferror(f_)) return false;
            if (!growing_) return false;
            if (writerDone_ && *writerDone_) return false;  // real EOF
            std::clearerr(f_);
            ::usleep(2000);
        }
        dataRead_ += want;
        l = sample(buf);
        r = (ch_ >= 2) ? sample(buf + bytesPerSample_) : l;
        return true;
    }

private:
    static std::uint16_t le16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
    static std::uint32_t le32(const unsigned char* p) {
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    }

    float sample(const unsigned char* p) const {
        if (fmt_ == 3) {  // IEEE float
            if (bits_ == 32) {
                float v;
                std::memcpy(&v, p, 4);
                return v;
            }
            if (bits_ == 64) {
                double v;
                std::memcpy(&v, p, 8);
                return static_cast<float>(v);
            }
            return 0.0f;
        }
        // PCM integer
        switch (bits_) {
            case 16: {
                const std::int16_t v = static_cast<std::int16_t>(le16(p));
                return static_cast<float>(v) / 32768.0f;
            }
            case 24: {
                std::int32_t v = static_cast<std::int32_t>(p[0]) |
                                 (static_cast<std::int32_t>(p[1]) << 8) |
                                 (static_cast<std::int32_t>(p[2]) << 16);
                if (v & 0x800000) v |= ~0xFFFFFF;
                return static_cast<float>(v) / 8388608.0f;
            }
            case 32: {
                const std::int32_t v = static_cast<std::int32_t>(le32(p));
                return static_cast<float>(static_cast<double>(v) / 2147483648.0);
            }
            default:
                return 0.0f;
        }
    }

    std::FILE* f_ = nullptr;
    std::uint16_t fmt_ = 1, ch_ = 0, bits_ = 0;
    std::uint32_t rate_ = 0, dataBytes_ = 0;
    bool growing_ = false;
    const std::atomic<bool>* writerDone_ = nullptr;
    long dataStart_ = 0, dataRead_ = 0;
    int bytesPerSample_ = 0;
    long frameBytes_ = 0;
};

}  // namespace pib
