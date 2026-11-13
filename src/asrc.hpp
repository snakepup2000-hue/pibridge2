// Arbitrary-rate windowed-sinc sample-rate converter with a live ratio trim.
//
// One kernel does both jobs: the file-rate to DAC-rate conversion, and the
// few-parts-per-million correction that keeps the Pi's ALSA buffer at a fixed
// depth. Because the trim is a rate ratio rather than a block splice, a ±500 ppm
// correction is a pitch shift of under one cent and cannot click.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace pib {

inline double besselI0(double x) {
    double sum = 1.0, term = 1.0, k = 1.0;
    const double x2 = x * x / 4.0;
    for (int i = 0; i < 256; ++i) {
        term *= x2 / (k * k);
        sum += term;
        if (term < sum * 1e-17) break;
        k += 1.0;
    }
    return sum;
}

class Asrc {
public:
    Asrc(double inRate, double outRate, int taps = 96, int phases = 512, double rolloff = 0.95)
        : inRate_(inRate), outRate_(outRate), taps_(taps), phases_(phases) {
        half_ = taps_ / 2;
        const double designRatio = outRate / inRate;
        // Cutoff in cycles per *input* sample: never above the input Nyquist
        // (0.5), and lower when decimating so nothing folds back.
        fc_ = 0.5 * std::min(1.0, designRatio) * rolloff;
        buildTable();
        step_ = inRate_ / outRate_;
        bufL_.assign(static_cast<std::size_t>(taps) * 2, 0.0f);
        bufR_.assign(static_cast<std::size_t>(taps) * 2, 0.0f);
        // Prime with a half-kernel of silence so the first output sample has a
        // complete window behind it. Half a kernel of input samples at the
        // source rate is sub-millisecond.
        for (int i = 0; i < half_; ++i) push(0.0f, 0.0f);
        readPos_ = static_cast<double>(half_);
    }

    void push(float l, float r) {
        bufL_.push_back(l);
        bufR_.push_back(r);
    }

    std::size_t inputPending() const {
        return bufL_.size() - static_cast<std::size_t>(std::max(0.0, readPos_));
    }

    // Pull one output frame. Returns false when more input is needed.
    bool produce(float& outL, float& outR) {
        if (readPos_ + static_cast<double>(taps_) > static_cast<double>(bufL_.size()))
            return false;

        const long k = static_cast<long>(std::floor(readPos_));
        const double f = readPos_ - static_cast<double>(k);
        const double p = f * static_cast<double>(phases_);
        int i0 = static_cast<int>(p);
        if (i0 < 0) i0 = 0;
        if (i0 > phases_ - 1) i0 = phases_ - 1;
        const float pf = static_cast<float>(p - static_cast<double>(i0));

        const float* w0 = &table_[static_cast<std::size_t>(i0) * taps_];
        const float* w1 = &table_[static_cast<std::size_t>(i0 + 1) * taps_];
        const std::size_t base = static_cast<std::size_t>(k - half_ + 1);
        const float* xl = &bufL_[base];
        const float* xr = &bufR_[base];

        float al = 0.0f, ar = 0.0f;
        for (int j = 0; j < taps_; ++j) {
            const float w = w0[j] + (w1[j] - w0[j]) * pf;
            al += xl[j] * w;
            ar += xr[j] * w;
        }
        outL = al;
        outR = ar;

        readPos_ += step_;
        if (readPos_ > static_cast<double>(taps_) * 8.0) compact();
        return true;
    }

    // Trim the conversion ratio. 1.0 == the nominal design rate.
    void setTrim(double trim) { step_ = (inRate_ / outRate_) / trim; }

    double ratio() const { return outRate_ / inRate_; }

private:
    void compact() {
        const std::size_t n = static_cast<std::size_t>(readPos_) - static_cast<std::size_t>(half_);
        bufL_.erase(bufL_.begin(), bufL_.begin() + static_cast<long>(n));
        bufR_.erase(bufR_.begin(), bufR_.begin() + static_cast<long>(n));
        readPos_ -= static_cast<double>(n);
    }

    void buildTable() {
        table_.assign(static_cast<std::size_t>(phases_ + 1) * taps_, 0.0f);
        const double twoFC = 2.0 * fc_;
        const double beta = 10.5;
        const double invI0 = 1.0 / besselI0(beta);

        for (int p = 0; p <= phases_; ++p) {
            const double f = static_cast<double>(p) / static_cast<double>(phases_);
            double sum = 0.0;
            for (int j = 0; j < taps_; ++j) {
                const double m = static_cast<double>(j - half_ + 1);
                const double u = f - m;
                const double au = std::fabs(u);
                double w = 0.0;
                if (au <= static_cast<double>(half_)) {
                    const double sincArg = twoFC * u;
                    const double s = (std::fabs(sincArg) < 1e-12)
                                         ? 1.0
                                         : std::sin(M_PI * sincArg) / (M_PI * sincArg);
                    const double t = u / static_cast<double>(half_);
                    const double win = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - t * t))) * invI0;
                    w = twoFC * s * win;
                }
                table_[static_cast<std::size_t>(p) * taps_ + j] = static_cast<float>(w);
                sum += w;
            }
            if (sum > 0.0) {
                const float norm = static_cast<float>(1.0 / sum);
                float* row = &table_[static_cast<std::size_t>(p) * taps_];
                for (int j = 0; j < taps_; ++j) row[j] *= norm;
            }
        }
    }

    double inRate_, outRate_, fc_, step_;
    int taps_, phases_, half_;
    std::vector<float> table_;
    std::vector<float> bufL_, bufR_;
    double readPos_ = 0.0;
};

}  // namespace pib
