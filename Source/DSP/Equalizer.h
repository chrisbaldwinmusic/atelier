#pragma once

#include "Common.h"

namespace atelier
{
/** Five-band program EQ: LF shelf, two parametric mids, HF shelf and an Air shelf. */
struct Equalizer
{
    struct Params
    {
        float lfGain = 0, lfFreq = 100, lmGain = 0, lmFreq = 400, lmQ = 0.9f;
        float hmGain = 0, hmFreq = 3000, hmQ = 0.9f, hfGain = 0, hfFreq = 8000;
        float airGain = 0, airFreq = 12000;
        int style = 0;      // 0 = Modern (constant Q), 1 = Vintage (proportional Q, broad shelves)
    };

    void prepare (double sampleRate)    { sr = sampleRate; reset(); }
    void reset() noexcept               { for (auto& b : bands) b.reset(); }

    void set (const Params& p) noexcept
    {
        const bool vintage = p.style == 1;
        const double shelfQ = vintage ? 0.5 : 0.707;
        auto bellQ = [vintage] (float q, float gain)
        {
            return vintage ? (double) q * (0.5 + std::abs ((double) gain) / 10.0) : (double) q;
        };

        bands[0].setLowShelf  (sr, p.lfFreq, shelfQ, p.lfGain);
        bands[1].setPeak      (sr, p.lmFreq, bellQ (p.lmQ, p.lmGain), p.lmGain);
        bands[2].setPeak      (sr, p.hmFreq, bellQ (p.hmQ, p.hmGain), p.hmGain);
        bands[3].setHighShelf (sr, p.hfFreq, shelfQ, p.hfGain);
        bands[4].setHighShelf (sr, p.airFreq, 0.45, p.airGain);
    }

    /** @param msMode 0 = stereo, 1 = mid only, 2 = side only. */
    void process (float* l, float* r, int n, int msMode) noexcept
    {
        if (msMode == 0)
        {
            for (int i = 0; i < n; ++i)
            {
                float a = l[i], b = r[i];
                for (auto& f : bands) { a = f.process (0, a); b = f.process (1, b); }
                l[i] = a; r[i] = b;
            }
            return;
        }

        for (int i = 0; i < n; ++i)
        {
            float m = 0.5f * (l[i] + r[i]), s = 0.5f * (l[i] - r[i]);
            float& target = msMode == 1 ? m : s;
            for (auto& f : bands) target = f.process (0, target);
            l[i] = m + s; r[i] = m - s;
        }
    }

    double sr = 48000.0;
    Biquad bands[5];
};

//==============================================================================
/** Mid/side width with a mono-maker for the low end. */
struct StereoImage
{
    void prepare (double sampleRate)    { sr = sampleRate; sideHp.reset(); }
    void reset() noexcept               { sideHp.reset(); }

    void set (float widthPct, float monoBassHz) noexcept
    {
        width = widthPct * 0.01f;
        monoOn = monoBassHz > 20.5f;
        if (monoOn) sideHp.setHighPass (sr, monoBassHz, 0.707);
    }

    void process (float* l, float* r, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (l[i] + r[i]);
            float s = 0.5f * (l[i] - r[i]);
            if (monoOn) s = sideHp.process (0, s);
            s *= width;
            l[i] = m + s; r[i] = m - s;
        }
    }

    double sr = 48000.0;
    float width = 1.0f;
    bool monoOn = false;
    Biquad sideHp;
};
} // namespace atelier
