#pragma once

#include "Common.h"

namespace atelier
{
/** An output transformer, solved as a circuit (referred to the primary, as a 1:1 transformer).

        in ──R1──┬──────── Ll ────────┬── out
                 │                    │
           magnetising  Rc            C      RL
            (the core)                │
                 │                    │
                gnd                  gnd

    R1 is the driving stage's output impedance plus the primary winding, Ll the leakage inductance, C the
    winding capacitance and RL the load. The core is the magnetising branch: its current depends on the flux,
    the running integral of the voltage across it, so it is the low frequencies and the loud passages that
    push the core towards saturation. The distortion comes from that magnetising current flowing back through
    R1, which is why the same core sounds cleaner driven from a low impedance.

    The core's curve (current against flux) has three parts: an initial region where the permeability is still
    rising, which adds a little low-level grit (fading as 1 / (1 + (flux / lambda1)^2)^2); the linear middle; and saturation. A single-ended class-A stage
    also passes its standing current through the primary, biasing the core off-centre, which is where that kind
    of console gets its 2nd harmonic.

    Hysteresis: moving the core's magnetic domains takes a little extra current, the coercive current hc, in
    whichever direction the flux is going. When the flux turns round, that current swings across to the other
    side over a flux width w, following dh/dt = (v hc - |v| h) / w, where v is the voltage across the core (the
    rate of change of flux). Large swings trace the full loop, ±hc wide; swings small next to w trace small loops
    whose width grows with their size, which is where transformers get their low-level odd harmonics and why the
    lowest notes come out a touch rougher than a single curve would make them. The loop takes energy from every
    cycle, the core's hysteresis loss. Nickel alloys are chosen for their tiny coercivity; steel has more.

    Each sample the linear network is reduced to how the core's voltage depends on its current, which leaves
    one equation in the flux. Its slope is always at least 1, so Newton's method converges in a step or two. */
struct IronCircuit
{
    struct Core
    {
        double R1, RL, Rc;          // source + winding, load, core loss (ohms)
        double L0;                  // magnetising inductance in the linear region (henries)
        double lowLevel, lambda1;   // extra reluctance at low flux, and the flux it fades out by
        double lambdaS;             // saturation flux (volt-seconds)
        int knee;                   // how sharply saturation arrives (the power of the flux it grows with)
        double f0, q;               // leakage/capacitance resonance and its damping, at the load
        double bias;                // standing flux from DC in the primary, as a fraction of lambdaS
        double hc;                  // coercive current, the hysteresis loop's half-width (amps)
        double width;               // flux over which the loop's current swings across, as a fraction of lambdaS
    };

    // Steel: driven from a high impedance, permeability still rising at low flux, a sharp knee.
    // Nickel: driven from a low impedance, high permeability, saturating earlier but more softly.
    // Console: a single-ended class-A stage, its standing current biasing the core off-centre.
    // Coercive currents: enough for a floor of 0.1 to 0.2 % at 40 Hz on steel at low level, ten times less on nickel.
    static constexpr Core steel   { 1500.0, 10.0e3, 50.0e3,  80.0, 1.0, 0.004, 0.09,  3, 70.0e3, 0.75, 0.0, 20.0e-6, 0.02 };
    static constexpr Core nickel  {  150.0, 10.0e3, 200.0e3, 120.0, 0.2, 0.002, 0.035, 2, 150.0e3, 0.7, 0.0, 2.0e-6, 0.02 };
    static constexpr Core console {  600.0, 10.0e3, 80.0e3,  60.0, 0.6, 0.003, 0.06,  2, 50.0e3, 0.9, 0.5, 15.0e-6, 0.02 };

    void prepare (double sampleRate)    { T = 1.0 / sampleRate; setCore (steel); }

    void setCore (const Core& c)
    {
        core = c;
        Ll = c.RL / (c.q * 2.0 * kPi * c.f0);
        C  = c.q / (c.RL * 2.0 * kPi * c.f0);
        gL = T / (2.0 * Ll);
        gC = 2.0 * C / T;
        hw = c.width * c.lambdaS;

        // the standing flux, and the DC magnetising current that holds it there
        lambda0 = c.bias * c.lambdaS;
        double d;
        idc = magnetising (lambda0, d);

        // the network seen by the core, as vm = A + B * (magnetising current), with the sources still to come:
        // node m: vm (1/R1 + 1/Rc + gL) - gL vo = vin/R1 - hL - j
        // node o: -gL vm + vo (gL + gC + 1/RL) = hL + hC
        const double a11 = 1.0 / c.R1 + 1.0 / c.Rc + gL, a12 = -gL, a21 = -gL, a22 = gL + gC + 1.0 / c.RL;
        const double det = a11 * a22 - a12 * a21;
        i11 = a22 / det; i12 = -a12 / det; i21 = -a21 / det; i22 = a11 / det;

        // small-signal gain in the middle of the band, where the core and the resonance are out of the way
        const double shunt = 1.0 / (1.0 / c.Rc + 1.0 / c.RL);
        midGain = shunt / (c.R1 + shunt);
        setInputScale (inScale);
        reset();
    }

    void reset() noexcept
    {
        double slope;
        magnetising (lambda0, slope);
        for (auto& s : st) s = State { lambda0, 0.0, 0.0, 0.0, 0.0, slope, 0.0, 0.0 };
    }

    /** Volts at the input for a full-scale sample. */
    void setInputScale (double voltsPerUnit) noexcept   { inScale = voltsPerUnit; outScale = 1.0 / (midGain * voltsPerUnit); }

    inline float process (int ch, float x) noexcept     { return (float) (processVolts (ch, (double) x * inScale) * outScale); }

    /** One sample, in volts at the input and at the load. */
    inline double processVolts (int ch, double vin) noexcept
    {
        auto& s = st[ch];

        // with the magnetising current j as a source, vm = A + B j and vo = Co + D j
        const double r1 = vin / core.R1 - s.hL, r2 = s.hL + s.hC;
        const double A = i11 * r1 + i12 * r2, B = -i11;
        const double Co = i21 * r1 + i22 * r2, D = -i21;

        // flux: lambda = lambda_prev + T/2 (vm + vm_prev) = base - k j, with the current j depending on the flux
        const double base = s.lambda + 0.5 * T * (s.vm + A), k = -0.5 * T * B;      // k >= 0, as B < 0
        // The first Newton step is free: it starts from the last sample's flux, current and slope. Each later
        // step evaluates the core afresh; once a step is this small the current is carried across it, an error of
        // second order in the step.
        double lambda = s.lambda, j = s.j, slope = s.slope, h = s.h;
        const double tol = 1.0e-7 * core.lambdaS;
        for (int it = 0; it < 8; ++it)
        {
            if (it > 0)
            {
                double dh;
                h = loop (s, lambda, dh);
                j = magnetising (lambda, slope) + h - idc;
                slope += dh;
            }
            const double step = (lambda - base + k * j) / (1.0 + k * slope);
            lambda -= step;
            j -= slope * step;
            if (it > 0 && std::abs (step) < tol) break;
        }
        if (core.hc > 0.0)
        {
            // settle the loop's current exactly at the final flux, and keep its rate for the next step
            double dh;
            const double vNow = 2.0 * (lambda - s.lambda) / T - s.vm;
            s.h = loop (s, lambda, dh);
            s.hRate = (vNow * core.hc - std::abs (vNow) * s.h) / hw;
        }
        s.j = j; s.slope = slope;

        const double vm = A + B * j, vo = Co + D * j;
        const double iL = gL * (vm - vo) + s.hL, iC = gC * vo - s.hC;
        s.hL = iL + gL * (vm - vo);
        s.hC = gC * vo + iC;
        s.lambda = lambda; s.vm = vm;
        return vo;
    }

    /** Magnetising current for a flux, and its slope. */
    inline double magnetising (double lambda, double& slope) const noexcept
    {
        // current = lambda * g(lambda) / L0, with g = 1 + low-level excess + saturation;
        // the low-level excess fades as 1 / (1 + x^2)^2
        const double x = lambda / core.lambda1, x2 = x * x, w = 1.0 / (1.0 + x2);
        const double e = core.lowLevel * w * w;
        const double a = std::abs (lambda) / core.lambdaS;
        double am1 = 1.0;                                                        // a^(knee - 1), by multiplication
        for (int k = 1; k < core.knee; ++k) am1 *= a;
        const double sat = am1 * a;
        const double g = 1.0 + e + sat;
        // d/dlambda [lambda g] = g + lambda g', where lambda g' = -4 x^2 e / (1 + x^2) + knee * sat
        slope = (g - 4.0 * x2 * e * w + (double) core.knee * sat) / core.L0;
        return lambda * g / core.L0;
    }

    double gain() const noexcept    { return midGain; }

    /** The hysteresis current once the flux has moved to `lambda` this step (trapezoidal rule, as for the rest),
        and its slope with respect to the flux. Given the flux it is linear in the current, so it comes out
        directly; its slope is never negative, so it can only steady Newton's method. */
    template <typename S>
    inline double loop (const S& s, double lambda, double& dh) const noexcept
    {
        if (core.hc <= 0.0) { dh = 0.0; return 0.0; }
        const double v = 2.0 * (lambda - s.lambda) / T - s.vm, half = 0.5 * T / hw;
        const double num = s.h + 0.5 * T * s.hRate + half * v * core.hc, den = 1.0 + half * std::abs (v);
        const double h = num / den;
        dh = (core.hc - (v >= 0.0 ? h : -h)) / (hw * den);
        return h;
    }

    Core core = steel;

private:
    struct State { double lambda, vm, hL, hC, j, slope, h, hRate; };    // j: magnetising current less the standing DC; h: hysteresis

    double T = 1.0 / 384000.0, Ll = 0.03, C = 1.0e-10, gL = 0.0, gC = 0.0, hw = 1.0;
    double lambda0 = 0.0, idc = 0.0;
    double i11 = 0.0, i12 = 0.0, i21 = 0.0, i22 = 0.0, midGain = 1.0;
    double inScale = 1.0, outScale = 1.0;
    State st[2] {};
};
} // namespace atelier
