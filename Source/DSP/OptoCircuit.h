#pragma once

#include "Common.h"
#include <algorithm>
#include <cmath>

namespace atelier
{
//==============================================================================
/** The gain element of an optical compressor: an electroluminescent panel shining on a light-dependent resistor.

        in --- Rs ---+--- out           side chain --> [ panel ] ~~light~~> [ CdS cell ]
                     |
                  CdS cell
                     |
                    gnd

    The audio passes a plain divider: a series resistor and the photoresistive cell to ground. Nothing in the audio
    path is non-linear (a CdS cell is ohmic at audio levels), so this stage is clean; everything it does, it does in
    time. The side chain lights the panel and the cell's resistance falls as it is lit.

    Panel: an electroluminescent phosphor's brightness rises steeply with the voltage across it, as exp(-b / sqrt V)
    (the Alfrey-Taylor law), and the phosphor keeps glowing for about a millisecond after the drive drops.

    Cell: light frees charge carriers in the cadmium sulphide, and the conductance is proportional to how many are
    free. They disappear in two ways: slowly on their own (a lifetime of seconds), and by meeting one another, which
    is faster the more of them there are. That second way is what gives an optical compressor its release: quick
    from heavy reduction, then slower and slower as the cell darkens. Some carriers fall into traps in the crystal
    and are let out over several seconds. The traps fill only under long, heavy light, so after a sustained passage
    the last decibel or so of reduction lingers, and after a brief peak it doesn't: the cell's memory.

    At rest everything is dark: no light, no free carriers, the traps empty.

    The numbers are this project's own, chosen to give the timing optical compressors are known for (attack around
    10 ms, the first half of the release in about 60 ms, the tail over seconds); they are not from a datasheet.

    Variables, in the cell: x = Rs times the conductance the light has added (so the divider's gain is
    1 / (1 + x + xd)), z = how full the traps are (0 to 1). The side chain's attack and release knobs scale the
    cell's timing while it is lighting up and while it is going dark; how far it settles is unchanged by them. */
struct OptoCell
{
    static constexpr double Rs = 47.0e3, Rdark = 5.0e6, xd = Rs / Rdark;
    static constexpr double panelB = 15.0, panelTau = 1.0e-3;                  // Alfrey-Taylor constant (V^0.5), phosphor afterglow
    static constexpr double gain = 1.8e5, lifetime = 2.0, pairRate = 5.5;      // generation per unit light, slow loss, pair loss
    static constexpr double capture = 0.05, trapCap = 1.0, trapTime = 4.0;     // trapping rate, trap capacity, time to be let out
    static constexpr double maxLight = 0.9;

    void prepare (double sampleRate)
    {
        h = 1.0 / sampleRate;
        reset();
    }

    void reset() noexcept
    {
        L = x = z = a = 0.0;
        fx = fz = 0.0;
    }

    /** How much faster than its own the cell moves while lighting up (attack) and going dark (release). */
    void setTiming (double attackScale, double releaseScale) noexcept   { sa = attackScale; sr = releaseScale; }

    /** Panel brightness for a drive voltage. */
    static inline double panel (double volts) noexcept
    {
        return volts > 1.0e-3 ? std::exp (-panelB / std::sqrt (volts)) : 0.0;
    }

    /** The drive voltage that, held, settles the cell at a given gain reduction. */
    static double voltsFor (double reductionDb) noexcept
    {
        if (reductionDb <= 0.0) return 0.0;
        const double xs = (1.0 + xd) * std::pow (10.0, reductionDb / 20.0) - 1.0 - xd;
        const double light = std::min (maxLight, (xs / lifetime + pairRate * xs * xs) / gain);
        if (light <= 0.0) return 0.0;
        const double r = panelB / std::log (1.0 / light);
        return r * r;
    }

    /** Gain reduction (dB) for the cell's present state. */
    double reduction() const noexcept   { return 20.0 * std::log10 ((1.0 + x + xd) / (1.0 + xd)); }

    /** The divider's gain now, relative to the dark cell. */
    double relativeGain() const noexcept  { return (1.0 + xd) / (1.0 + x + xd); }

    /** The divider's output for an input voltage. */
    double divide (double vin) const noexcept  { return vin / (1.0 + x + xd); }

    /** Advances panel and cell by one sample with this drive voltage (trapezoidal rule, as ngspice). */
    inline void step (double volts) noexcept
    {
        // the phosphor: linear in its brightness, so its step is exact in closed form
        const double an = panel (volts), k = 0.5 * h / panelTau;
        L = (L * (1.0 - k) + k * (a + an)) / (1.0 + k);
        a = an;

        // the cell: Newton's method on the two trapezoidal equations
        const double gl = gain * L, x0 = x, z0 = z, fx0 = fx, fz0 = fz, hh = 0.5 * h;
        double x1 = std::max (0.0, x0 + h * fx0), z1 = z0;
        double fx1 = 0.0, fz1 = 0.0;
        for (int it = 0; it < 30; ++it)
        {
            double dxx, dxz, dzx, dzz;
            rates (gl, x1, z1, fx1, fz1, dxx, dxz, dzx, dzz);
            const double F1 = x1 - x0 - hh * (fx0 + fx1), F2 = z1 - z0 - hh * (fz0 + fz1);
            const double j00 = 1.0 - hh * dxx, j01 = -hh * dxz, j10 = -hh * dzx, j11 = 1.0 - hh * dzz;
            const double inv = 1.0 / (j00 * j11 - j01 * j10);
            const double sx = (F1 * j11 - F2 * j01) * inv, sz = (F2 * j00 - F1 * j10) * inv;
            x1 -= sx; z1 -= sz;
            if (std::abs (sx) < 1.0e-12 * (1.0 + x1) && std::abs (sz) < 1.0e-12) break;
        }
        if (! std::isfinite (x1) || ! std::isfinite (z1)) { reset(); return; }

        x = std::max (0.0, x1); z = std::clamp (z1, 0.0, 1.0);
        double d0, d1, d2, d3;
        rates (gl, x, z, fx, fz, d0, d1, d2, d3);
    }

    double light() const noexcept  { return L; }
    double carriers() const noexcept  { return x; }
    double traps() const noexcept  { return z; }

private:
    /** The cell's rates of change and their slopes. */
    inline void rates (double gl, double xv, double zv, double& fxo, double& fzo,
                       double& dxx, double& dxz, double& dzx, double& dzz) const noexcept
    {
        const double trapIn = capture * xv * (1.0 - zv), trapOut = trapCap * zv / trapTime;
        const double raw = gl - xv / lifetime - pairRate * xv * xv - trapIn + trapOut;
        const double s = raw > 0.0 ? sa : sr;
        fxo = s * raw;
        dxx = s * (-1.0 / lifetime - 2.0 * pairRate * xv - capture * (1.0 - zv));
        dxz = s * (capture * xv + trapCap / trapTime);
        fzo = (trapIn - trapOut) / trapCap;
        dzx = capture * (1.0 - zv) / trapCap;
        dzz = (-capture * xv - trapCap / trapTime) / trapCap;
    }

    double h = 1.0 / 48000.0, sa = 1.0, sr = 1.0;
    double L = 0.0, x = 0.0, z = 0.0, a = 0.0, fx = 0.0, fz = 0.0;
};
} // namespace atelier
