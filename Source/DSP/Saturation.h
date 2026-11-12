#pragma once

#include "Common.h"
#include "TubeCircuit.h"

/*  Analog colour stages. All of these are designed to run at the oversampled rate.
    Each stage is normalised to unity small-signal gain: turning up the drive pushes
    the signal further into the curve instead of making it louder.
*/
namespace atelier
{
//==============================================================================
/** Valve preamp stage.
    Triode is a real 12AX7 gain stage solved as a circuit (see TubeCircuit.h) and checked
    against ngspice. Pentode and Push-Pull are still transfer curves with an adjustable bias point. */
struct TubeStage
{
    enum Type { triode = 0, pentode, pushPull };

    void prepare (double osRate)    { dc.setCutoff (osRate, 8.0); dc.reset(); circuit.prepare (osRate); }
    void reset() noexcept           { dc.reset(); circuit.reset(); }

    /** @param headroom  mode-dependent calibration (1 = channel, lower = cleaner). */
    void set (int newType, float driveDb, float bias01, float headroom) noexcept
    {
        type   = newType;
        drive  = dbToGain (driveDb) * headroom;

        if (type == triode)
        {
            // Bias chooses the cathode resistor, 600 ohms (hot, open) to 6k (cold, lopsided); 40 % is the textbook 1.5k.
            // Full scale at 0 dB of drive puts 0.2 V on the grid, so a 0 VU signal sits where the stage is just colouring.
            circuit.setCathodeResistor (600.0 * std::pow (10.0, (double) std::clamp (bias01, 0.0f, 1.0f)));
            circuit.setInputScale (0.2 * (double) drive);
            return;
        }

        offset = bias01 * (type == pushPull ? 0.15f : (type == pentode ? 0.35f : 0.5f));
        s0     = shape (offset);
        const float slope = (shape (offset + 1.0e-3f) - shape (offset - 1.0e-3f)) / 2.0e-3f;
        norm   = 1.0f / (slope * drive);
    }

    inline float shape (float u) const noexcept
    {
        switch (type)
        {
            case pentode:   { const float u2 = u * u; return u / std::sqrt (std::sqrt (1.0f + u2 * u2)); }
            case pushPull:  return std::tanh (u);
            default:        return u >= 0.0f ? std::tanh (u)             // grid-conduction side
                                             : u / (1.0f - 0.5f * u);    // soft cut-off side
        }
    }

    void process (float* l, float* r, int n) noexcept
    {
        if (type == triode)
        {
            // Left and right are worked through together, a sample of each in turn. Each channel's solve has to
            // wait for its own previous sample, but the two are independent, so the processor can overlap them.
            for (int i = 0; i < n; ++i)
            {
                l[i] = circuit.process (0, l[i]);
                r[i] = circuit.process (1, r[i]);
            }
            return;
        }

        float* chans[2] = { l, r };
        for (int ch = 0; ch < 2; ++ch)
        {
            float* x = chans[ch];
            for (int i = 0; i < n; ++i) x[i] = dc.hp (ch, (shape (drive * x[i] + offset) - s0) * norm);
        }
    }

    TubeCircuit circuit;
    int type = triode;
    float drive = 1.0f, offset = 0.0f, s0 = 0.0f, norm = 1.0f;
    OnePole dc;
};

//==============================================================================
/** Magnetic hysteresis (Jiles-Atherton model, solved with RK2 at the oversampled rate).
    Unlike a static curve, the magnetisation depends on where the tape has been: the
    loop has width, low-level signals ride on a different slope from loud ones, and
    transients are treated differently from sustained tones.

    `c` is the reversible fraction. A correctly biased machine sits close to the
    anhysteretic curve (c near 1); under-biasing lowers c, which widens the loop and
    brings out the gritty, level-dependent character of starved tape. */
struct Hysteresis
{
    void prepare (double sampleRate)    { T = 1.0 / sampleRate; reset(); }
    void reset() noexcept               { for (int i = 0; i < 2; ++i) M[i] = H1[i] = Hd1[i] = 0.0; }

    void set (double reversible) noexcept
    {
        c = std::clamp (reversible, 0.2, 0.98);
        // small-signal and loop-dominated slopes; output is normalised to their geometric mean
        const double chi = Ms / (3.0 * a);
        const double g0 = c * chi / (1.0 - c * alpha * chi);
        norm = 1.0 / (g0 / std::sqrt (c));
    }

    inline double process (int ch, double H) noexcept
    {
        constexpr double dAlpha = 0.75;     // damped trapezoidal differentiator
        const double Hd = ((1.0 + dAlpha) / T) * (H - H1[ch]) - dAlpha * Hd1[ch];

        const double k1 = T * slope (M[ch], H1[ch], Hd1[ch]);
        const double k2 = T * slope (M[ch] + 0.5 * k1, 0.5 * (H + H1[ch]), 0.5 * (Hd + Hd1[ch]));
        double m = M[ch] + k2;
        if (! (std::abs (m) <= Ms)) m = std::isfinite (m) ? std::clamp (m, -Ms, Ms) : 0.0;

        M[ch] = m; H1[ch] = H; Hd1[ch] = Hd;
        return m * norm;
    }

private:
    inline double slope (double m, double h, double hd) const noexcept
    {
        const double q = (h + alpha * m) / a;
        double L, Ld;
        if (std::abs (q) < 0.3)     // Taylor series of the Langevin function: exact to 1e-8 here, and much cheaper
        {
            const double q2 = q * q;
            L  = q * (1.0 / 3.0 - q2 * (1.0 / 45.0 - q2 * (2.0 / 945.0 - q2 * (1.0 / 4725.0))));
            Ld = 1.0 / 3.0 - q2 * (1.0 / 15.0 - q2 * (2.0 / 189.0 - q2 * (1.0 / 675.0)));
        }
        else
        {
            const double e = std::exp (-2.0 * std::abs (q));
            const double coth = std::copysign ((1.0 + e) / (1.0 - e), q);
            L  = coth - 1.0 / q;
            Ld = 1.0 / (q * q) - coth * coth + 1.0;
        }

        const double diff  = Ms * L - m;                    // distance to the anhysteretic curve
        const double dir   = hd >= 0.0 ? 1.0 : -1.0;
        const double irrev = (dir * diff > 0.0) ? (1.0 - c) * diff / ((1.0 - c) * dir * k - alpha * diff) : 0.0;
        const double rev   = c * (Ms / a) * Ld;
        return hd * (irrev + rev) / (1.0 - alpha * rev);
    }

    static constexpr double Ms = 1.0, a = 0.3, alpha = 1.6e-3, k = 0.47875;
    double T = 1.0 / 384000.0, c = 0.9, norm = 1.0;
    double M[2] {}, H1[2] {}, Hd1[2] {};
};

//==============================================================================
/** Tape machine: record pre-emphasis -> magnetic hysteresis -> playback de-emphasis,
    with speed-dependent head bump and bandwidth. Because of the emphasis the top end
    reaches saturation first, as on a real machine. */
struct TapeStage
{
    void prepare (double osRate)
    {
        sr = osRate;
        hysteresis.prepare (osRate);
        reset();
    }

    void reset() noexcept
    {
        hysteresis.reset();
        for (auto* f : { &pre, &post, &bump, &hp, &lp }) f->reset();
    }

    /** @param speed 0 = 7.5 ips, 1 = 15 ips, 2 = 30 ips.  @param bias -1 (under) .. +1 (over). */
    void set (int speed, float driveDb, float bias, float bump01, float headroom) noexcept
    {
        static constexpr double bumpHz[3] = { 45.0, 70.0, 120.0 };
        static constexpr double topHz[3]  = { 13000.0, 19500.0, 27000.0 };
        speed = std::clamp (speed, 0, 2);

        const double emphasis = 6.0 - 3.0 * (double) bias;
        pre.setHighShelf  (sr, 3500.0, 0.6,  emphasis);
        post.setHighShelf (sr, 3500.0, 0.6, -emphasis);
        bump.setPeak      (sr, bumpHz[speed], 1.1, (double) bump01 * 4.0);
        hp.setHighPass    (sr, bumpHz[speed] * 0.35, 0.707);
        lp.setLowPass     (sr, topHz[speed] * (1.0 - 0.18 * std::max (0.0, (double) bias)), 0.6);

        // correct bias sits near the anhysteretic curve; under-bias widens the loop
        hysteresis.set (bias < 0.0f ? 0.88 + 0.48 * (double) bias : 0.88 + 0.10 * (double) bias);

        drive = dbToGain (driveDb) * headroom * 0.9f;
        inv   = 1.0f / drive;
    }

    void process (float* l, float* r, int n) noexcept
    {
        // channels interleaved for the same reason as the valve stage: two independent chains overlap
        float* chans[2] = { l, r };
        for (int i = 0; i < n; ++i)
            for (int ch = 0; ch < 2; ++ch)
            {
                float* x = chans[ch];
                const float u = pre.process (ch, x[i] * drive);
                float y = post.process (ch, (float) hysteresis.process (ch, (double) u)) * inv;
                y = bump.process (ch, y);
                y = hp.process (ch, y);
                x[i] = lp.process (ch, y);
            }
    }

    double sr = 384000.0;
    float drive = 1.0f, inv = 1.0f;
    Hysteresis hysteresis;
    Biquad pre, post, bump, hp, lp;
};

//==============================================================================
/** Output iron / console stage: the core saturates on low-frequency flux first,
    so bass thickens before the top end is touched. */
struct IronStage
{
    enum Type { steel = 0, nickel, console };

    void prepare (double osRate)
    {
        sr = osRate;
        dc.setCutoff (osRate, 6.0);
        reset();
    }

    void reset() noexcept   { dc.reset(); sPre[0] = sPre[1] = sPost[0] = sPost[1] = 0.0; }

    void set (int type, float driveDb, float headroom) noexcept
    {
        static constexpr double corner[3] = { 180.0, 90.0, 120.0 };
        static constexpr double lfAmt[3]  = { 2.0, 0.8, 1.2 };
        static constexpr float  even[3]   = { 0.05f, 0.02f, 0.16f };
        type  = std::clamp (type, 0, 2);
        a     = 1.0 - std::exp (-2.0 * kPi * corner[type] / sr);
        c     = lfAmt[type];
        e     = even[type];
        drive = dbToGain (driveDb) * headroom * 0.7f;
        inv   = 1.0f / drive;
    }

    void process (float* l, float* r, int n) noexcept
    {
        float* chans[2] = { l, r };
        const double k = 1.0 / (1.0 + c * a);

        for (int ch = 0; ch < 2; ++ch)
        {
            float* x = chans[ch];
            for (int i = 0; i < n; ++i)
            {
                // flux emphasis: w = x + c * lowpass(x)
                sPre[ch] += a * (x[i] - sPre[ch]);
                const float u  = drive * (float) (x[i] + c * sPre[ch]);
                const float u2 = u * u;
                const float v  = (std::tanh (u) + e * u2 / (1.0f + u2)) * inv;

                // exact inverse of the emphasis filter
                const double y = (v - c * sPost[ch] * (1.0 - a)) * k;
                sPost[ch] += a * (y - sPost[ch]);
                x[i] = dc.hp (ch, (float) y);
            }
        }
    }

    double sr = 192000.0, a = 0.01, c = 1.0;
    double sPre[2] {}, sPost[2] {};
    float e = 0.05f, drive = 1.0f, inv = 1.0f;
    OnePole dc;
};
//==============================================================================
/** Soft clipper used ahead of the limiter. Short peaks are rounded off instead of being
    turned down, which keeps drums punchy where a limiter alone would duck everything.
    `amount` blends it in: 0 leaves every peak to the limiter, 1 clips them all. */
struct SoftClip
{
    /** Unity below the knee, then eases into a ceiling of 1. */
    static inline float curve (float u) noexcept
    {
        constexpr float knee = 0.75f;
        const float a = std::abs (u);
        if (a <= knee) return u;
        return std::copysign (knee + (1.0f - knee) * std::tanh ((a - knee) / (1.0f - knee)), u);
    }

    /** Returns the largest peak reduction in the block, in dB. */
    static float process (float* l, float* r, int n, float threshold, float amount) noexcept
    {
        const float inv = 1.0f / threshold;
        float in = 1.0e-9f, out = 1.0e-9f;
        float* chans[2] = { l, r };
        for (auto* x : chans)
            for (int i = 0; i < n; ++i)
            {
                const float y = x[i] + amount * (curve (x[i] * inv) * threshold - x[i]);
                in = std::max (in, std::abs (x[i])); out = std::max (out, std::abs (y));
                x[i] = y;
            }
        return gainToDb (in) - gainToDb (out);
    }
};
} // namespace atelier
