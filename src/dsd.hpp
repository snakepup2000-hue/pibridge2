// 4th-order 1-bit delta-sigma modulator, ported from SignalPath's
// Sources/SignalPathCore/DsdModulator.swift (117-120 dB in-band at OSR 128).
//
// For native DSD_U32_BE each 32-bit word carries 32 consecutive 1-bit samples,
// oldest bit in the MSB, so one word == one modulator run of 32 steps.

#pragma once

#include <cstdint>

namespace pib {

struct Sdm {
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    bool last = true;
    int run = 0;

    static constexpr double a1 = 0.10, a2 = 0.30, a3 = 0.30, a4 = 0.30;

    void reset() {
        s0 = s1 = s2 = s3 = 0.0;
        run = 0;
        last = true;
    }

    // One DSD bit. Input gets 6 dB of headroom and a clamp; the run-length
    // guard resets the integrators if the loop ever overloads.
    inline int step(double input) {
        double x = input * 0.5;
        if (x > 0.5) x = 0.5;
        else if (x < -0.5) x = -0.5;

        const double yi = (s3 >= 0.0) ? 1.0 : -1.0;
        const bool bit = yi > 0.0;

        if (bit == last) ++run;
        else { run = 0; last = bit; }
        if (run > 64) { s0 = s1 = s2 = s3 = 0.0; run = 0; }

        double prev = x - yi;
        s0 += a1 * prev; prev = s0 - yi;
        s1 += a2 * prev; prev = s1 - yi;
        s2 += a3 * prev; prev = s2 - yi;
        s3 += a4 * prev;
        return bit ? 1 : 0;
    }

    // 32 bits, first produced bit in the MSB (DSD_U32_BE, bitrev=0).
    inline std::uint32_t word32(double x) {
        std::uint32_t w = 0;
        for (int i = 0; i < 32; ++i) w = (w << 1) | static_cast<std::uint32_t>(step(x));
        return w;
    }
};

}  // namespace pib
