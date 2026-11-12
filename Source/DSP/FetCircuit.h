#pragma once

#include "Common.h"
#include <algorithm>
#include <cmath>

namespace atelier
{
//==============================================================================
/** The gain element of a FET compressor: a junction FET shunting the signal to ground, as a voltage-controlled
    resistor.

        in --- Rs ---+--- drain ------------------------- out
                     |     |
                   JFET    Cf
                     |     |
                    gnd    Rf
                           |
                           +--- gate --- Rc --- control (from the side chain, negative)

    With its gate well below pinch-off the FET is open and the signal passes untouched. The side chain raises the
    gate towards zero and the channel's resistance falls, so the divider turns the signal down: about 250 ohms at
    full conduction against the 22 k series resistor, so up to 39 dB of reduction.

    A FET's channel is only a resistor while the voltage across it is small next to how far the gate is above
    pinch-off; beyond that it saturates and distorts. Classic FET compressors feed half the drain voltage back to
    the gate (here through Cf and Rf, with Rf equal to Rc), which cancels the channel's square-law term, so below
    saturation it is clean. Cf keeps the control voltage's DC off the drain, where it would be heard as a thump
    every time the gain moved. Saturation comes soonest when the gate is only just above pinch-off, which is where
    the compressor starts to bite: loud material is coloured there (odd harmonics, and a little extra reduction on
    the peaks), and turning the FET on harder for more reduction makes it cleaner again.

    Cf also slows how quickly a change of control voltage reaches the gate (by Rf + Rc times Cf, nearly a second),
    so the side chain sets its control from Cf's present charge to put the gate where it wants it at once.

    Whatever the control does also reaches the drain through Cf, scaled by about Rs / (Rf + Rc): it is heard as
    a low thump when the gain moves fast. That is why Rf and Rc are 10 M rather than the more usual 1 M (ten
    times less thump); the gate of a JFET draws so little current that they cost nothing else.

    The device is a 2N5457 in the Shichman-Hodges model (the same values the sibling app Lutherie uses), solved
    against ngspice's own JFET. The FET's few picofarads of capacitance are left out: with these resistances they
    act far above the audio band. */
struct FetCircuit
{
    static constexpr double beta = 1.125e-3, vto = -1.8, lambda = 0.0023;
    static constexpr double Rs = 22.0e3, Rf = 10.0e6, Rc = 10.0e6, Cf = 47.0e-9, Rt = Rf + Rc;
    static constexpr double restGate = vto - 0.5;                   // half a volt beyond pinch-off at rest
    static constexpr double maxReduction = 38.0;

    void prepare (double sampleRate)
    {
        gc = 2.0 * Cf * sampleRate;
        den = Rt * gc + 1.0;
        // at audio frequencies Cf is a short, so Rf + Rc load the drain a little: unity gain is measured from there
        restGain = 1.0 / (1.0 + Rs * gc / den);
        setInputScale (inScale);
        reset();
    }

    /** Volts at the divider's input for a full-scale sample. */
    void setInputScale (double voltsPerUnit) noexcept   { inScale = voltsPerUnit; outScale = 1.0 / (restGain * voltsPerUnit); }

    /** Silence, with the control held at `control` long enough for Cf to have settled. */
    void reset (double control = restGate) noexcept
    {
        for (auto& s : st) { s.vd = 0.0; s.hist = -gc * control; }
    }

    /** The gate voltage, with no signal, that gives a small-signal gain reduction (dB): there the channel conducts
        2 beta (vg - vto). */
    static double gateFor (double reductionDb) noexcept
    {
        if (reductionDb <= 0.0) return restGate;
        const double g = (std::pow (10.0, std::min (reductionDb, maxReduction) / 20.0) - 1.0) / Rs;
        const double vg = vto + g / (2.0 * beta);
        // Over the last tenth of a dB the gate eases back down to its resting bias. Left at pinch-off, where a
        // side chain that is nearly let go would otherwise leave it, loud material would switch the channel on at
        // every peak and hold the level down by a dB or two.
        constexpr double fade = 0.1;
        return reductionDb < fade ? restGate + (vg - restGate) * reductionDb / fade : vg;
    }

    /** The control voltage that puts the gate at `vg` (with no signal) given Cf's charge now. */
    double controlForGate (int ch, double vg) const noexcept
    {
        // vg = vc + Rc (gc (0 - vc) - hist) / den
        return (vg + Rc * st[ch].hist / den) / (1.0 - Rc * gc / den);
    }

    /** One sample: `gateTarget` from gateFor(). Unity gain at rest. */
    inline float process (int ch, float x, double gateTarget) noexcept
    {
        return (float) (processVolts (ch, (double) x * inScale, controlForGate (ch, gateTarget)) * outScale);
    }

    /** One sample: volts at the input and the control voltage; returns the drain voltage. */
    inline double processVolts (int ch, double vin, double control) noexcept
    {
        auto& s = st[ch];
        // the current down Cf, Rf and Rc is linear in the drain voltage: i = k vd + i0
        const double k = gc / den, i0 = (-gc * control - s.hist) / den;
        const double gate0 = control + Rc * i0, gateSlope = Rc * k;

        double v = s.vd;
        for (int it = 0; it < 40; ++it)
        {
            double id, did;
            channel (v, gate0 + gateSlope * v, gateSlope, id, did);
            const double f = (vin - v) / Rs - id - (k * v + i0), df = -1.0 / Rs - did - k;
            const double step = std::clamp (f / df, -0.5, 0.5);
            v -= step;
            if (std::abs (step) < 1.0e-14 + 1.0e-12 * std::abs (v)) break;
        }
        if (! std::isfinite (v)) { reset (control); return 0.0; }

        // Cf's history for the next sample: i(n+1) = gc v(n+1) - (gc v(n) + i(n))
        const double i = k * v + i0;
        const double vAcross = v - (control + Rt * i);      // drain minus the node between Cf and Rf
        s.hist = gc * vAcross + i;
        s.vd = v;
        return v;
    }

    /** Drain current, and its slope with respect to the drain voltage, with the gate at vg (which moves with the
        drain by dvg). */
    static inline void channel (double vdv, double vg, double dvg, double& id, double& did) noexcept
    {
        // symmetric: whichever terminal is lower acts as the source
        const bool fwd = vdv >= 0.0;
        const double vgs = fwd ? vg : vg - vdv, vds = std::abs (vdv);
        const double dvgs = fwd ? dvg : dvg - 1.0;                          // d vgs / d vd
        const double dvds = fwd ? 1.0 : -1.0;                               // d vds / d vd
        const double vov = vgs - vto;
        double i = 0.0, gm = 0.0, gds = 0.0;
        if (vov > 0.0)
        {
            const double lam = 1.0 + lambda * vds;
            if (vds < vov)
            {
                i = beta * vds * (2.0 * vov - vds) * lam;
                gm = 2.0 * beta * vds * lam;
                gds = beta * (2.0 * vov - 2.0 * vds) * lam + beta * vds * (2.0 * vov - vds) * lambda;
            }
            else
            {
                i = beta * vov * vov * lam;
                gm = 2.0 * beta * vov * lam;
                gds = beta * vov * vov * lambda;
            }
        }
        id  = fwd ? i : -i;
        did = (fwd ? 1.0 : -1.0) * (gm * dvgs + gds * dvds);
    }

private:
    struct State { double vd = 0.0, hist = 0.0; };
    State st[2];
    double gc = 1.0, den = 1.0, restGain = 1.0, inScale = 1.0, outScale = 1.0;
};
} // namespace atelier
