#pragma once

#include "Common.h"

namespace atelier
{
/** A real valve gain stage, solved as a circuit rather than drawn as a curve.

        in --Cin--+--Rgs-- grid         B+ --Rp--+-- plate --Cout--+-- out
                  |                              |                 |
                 Rgl             cathode --+-- Rk               Rload
                  |                        +-- Ck                  |
                 gnd                      gnd                     gnd

    A 12AX7 triode (Koren's model, the same device values and equations as the ngspice deck
    this is checked against) in the classic common-cathode arrangement. Because the
    capacitors and the grid are part of the solution, it does the things a static curve
    cannot: when driven hard the grid draws current and charges the input capacitor, which
    shifts the bias and changes the tone for a moment after a loud hit, and the operating
    point moves with the signal instead of sitting still.

    Each sample the linear part of the circuit (resistors, and capacitors as trapezoidal
    companion models) is reduced ahead of time to a 2x2 problem in the two voltages the
    valve cares about, grid-cathode and plate-cathode, which Newton's method then solves. */
struct TubeCircuit
{
    // component values
    static constexpr double supply = 250.0, Rp = 100.0e3, Cin = 47.0e-9, Rgl = 1.0e6, Rgs = 33.0e3;
    static constexpr double Ck = 22.0e-6, Cout = 100.0e-9, Rload = 1.0e6;
    // 12AX7
    static constexpr double mu = 100.0, ex = 1.4, kg1 = 1060.0, kp = 600.0, kvb = 300.0, gridK = 1300.0;

    void prepare (double sampleRate)
    {
        T = 1.0 / sampleRate;
        table();        // built here, never on the audio thread
        Rk = -1.0;
        setCathodeResistor (1500.0);
        reset();
    }

    /** Puts every capacitor at its resting voltage, as if the stage had been on for a while. */
    void reset() noexcept
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& s = st[ch];
            s.egk = -restVk; s.epk = restVp - restVk;
            currents (s.egk, s.epk, s.ip, s.ig, s.dIpG, s.dIpP, s.dIg);
            s.hIn = 0.0;
            s.hK  = gcK * restVk;
            s.hO  = gcO * restVp;
        }
    }

    /** The cathode resistor sets the operating point: small = hot and open, large = cold and closer to cut-off. */
    void setCathodeResistor (double ohms)
    {
        if (std::abs (ohms - Rk) < 0.5) return;
        Rk = ohms;
        build();
    }

    /** Volts at the input for a full-scale sample. */
    void setInputScale (double voltsPerUnit) noexcept
    {
        inScale = voltsPerUnit;
        outScale = 1.0 / (smallSignalGain * voltsPerUnit);     // unity (and non-inverting) for small signals
    }

    double gain() const noexcept            { return smallSignalGain; }
    double restingCathode() const noexcept  { return restVk; }

    /** One sample through the stage; input and output are in the plugin's normalised units. */
    inline float process (int ch, float x) noexcept
    {
        return (float) (processVolts (ch, (double) x * inScale) * outScale);
    }

    /** One sample, in volts at the input and at the loaded output. */
    inline double processVolts (int ch, double vin) noexcept
    {
        auto& s = st[ch];
        const double u[5] = { vin, s.hIn, s.hK, s.hO, 1.0 };

        double p0 = 0.0, p1 = 0.0;
        for (int j = 0; j < 5; ++j) { p0 += P[0][j] * u[j]; p1 += P[1][j] * u[j]; }

        // Newton on F(x) = x - p - K i(x). The first step costs nothing: it reuses the valve currents and
        // slopes from the last evaluation, which lands close enough that one fresh evaluation usually finishes.
        double vgk = s.egk, vpk = s.epk, ip = s.ip, ig = s.ig;
        double dIpG = s.dIpG, dIpP = s.dIpP, dIg = s.dIg;
        bool settled = false;

        for (int it = 0; it < 14; ++it)
        {
            if (it > 0)
            {
                currents (vgk, vpk, ip, ig, dIpG, dIpP, dIg);
                s.egk = vgk; s.epk = vpk; s.ip = ip; s.ig = ig; s.dIpG = dIpG; s.dIpP = dIpP; s.dIg = dIg;
            }

            const double f0 = vgk - p0 - (K[0][0] * ip + K[0][1] * ig);
            const double f1 = vpk - p1 - (K[1][0] * ip + K[1][1] * ig);

            const double j00 = 1.0 - (K[0][0] * dIpG + K[0][1] * dIg), j01 = -K[0][0] * dIpP;
            const double j10 =      - (K[1][0] * dIpG + K[1][1] * dIg), j11 = 1.0 - K[1][0] * dIpP;
            const double det = j00 * j11 - j01 * j10;
            if (std::abs (det) < 1.0e-30) break;

            // keep each step sensible so the exponential in the valve model can't run away
            const double invDet = 1.0 / det;
            const double dg = std::clamp ((f0 * j11 - f1 * j01) * invDet, -1.5, 1.5);
            const double dp = std::clamp ((f1 * j00 - f0 * j10) * invDet, -60.0, 60.0);
            vgk -= dg; vpk -= dp;

            // After a fresh evaluation, a step this small leaves an error far below audibility (Newton's error
            // shrinks with the square of the step), so carry the currents across it instead of evaluating again.
            if (it > 0 && std::abs (dg) < stepTolerance && std::abs (dp) < 50.0 * stepTolerance)
            {
                ip = std::max (0.0, ip - dIpG * dg - dIpP * dp);
                ig = std::max (0.0, ig - dIg * dg);
                settled = true;
                break;
            }
        }

        if (! (std::isfinite (vgk) && std::isfinite (vpk))) { reset(); return 0.0; }

        if (! settled)
        {
            currents (vgk, vpk, ip, ig, dIpG, dIpP, dIg);
            s.egk = vgk; s.epk = vpk; s.ip = ip; s.ig = ig; s.dIpG = dIpG; s.dIpP = dIpP; s.dIg = dIg;
        }

        // capacitor voltages and the output follow linearly from the sources and the two valve currents
        double m[4];
        for (int r = 0; r < 4; ++r)
        {
            double acc = Mi[r][0] * ip + Mi[r][1] * ig;
            for (int j = 0; j < 5; ++j) acc += Mu[r][j] * u[j];
            m[r] = acc;
        }

        s.hIn = 2.0 * gcIn * m[0] - s.hIn;
        s.hK  = 2.0 * gcK  * m[1] - s.hK;
        s.hO  = 2.0 * gcO  * m[2] - s.hO;
        return m[3];
    }

private:
    /** Koren triode plate current, grid current, and their slopes. */
    static inline void currents (double vgk, double vpk, double& ip, double& ig, double& dIpG, double& dIpP, double& dIg) noexcept
    {
        const double root = std::sqrt (kvb + vpk * vpk);
        const double a = kp * (1.0 / mu + vgk / root);
        double L, sig;
        softplus (a, L, sig);

        const double e1 = vpk / kp * L;
        if (e1 > 0.0)
        {
            const double pw = (double) std::pow ((float) e1, (float) (ex - 1.0));     // single precision is ample here
            ip = 2.0 * pw * e1 / kg1;
            const double dIpE = 2.0 * ex * pw / kg1;
            dIpG = dIpE * (vpk * sig / root);
            dIpP = dIpE * (L / kp - sig * vgk * vpk * vpk / (root * root * root));
        }
        else { ip = dIpG = dIpP = 0.0; }

        if (vgk > 0.0)  { const double r = std::sqrt (vgk); ig = vgk * r / gridK; dIg = 1.5 * r / gridK; }
        else            { ig = dIg = 0.0; }
    }

    /** ln(1 + e^a) and its slope, from a table: this sits in the innermost loop, and the exponential and
        logarithm it replaces were most of the cost. Cubic interpolation keeps the error around 1e-9. */
    struct SoftplusTable
    {
        static constexpr int size = 4096;
        static constexpr double lo = -32.0, hi = 32.0, step = (hi - lo) / size;
        double value[size + 1], slope[size + 1];

        SoftplusTable()
        {
            for (int i = 0; i <= size; ++i)
            {
                const double a = lo + step * i, e = std::exp (a);
                value[i] = std::log1p (e);
                slope[i] = e / (1.0 + e);
            }
        }
    };

    static const SoftplusTable& table()     { static const SoftplusTable t; return t; }

    static inline void softplus (double a, double& L, double& sig) noexcept
    {
        if (a >= SoftplusTable::hi)  { L = a; sig = 1.0; return; }
        if (a <= SoftplusTable::lo)  { L = 0.0; sig = 0.0; return; }

        const auto& t = table();
        const double pos = (a - SoftplusTable::lo) / SoftplusTable::step;
        const int i = std::min ((int) pos, SoftplusTable::size - 1);
        const double x = pos - (double) i, x2 = x * x, x3 = x2 * x, h = SoftplusTable::step;

        L = (2.0 * x3 - 3.0 * x2 + 1.0) * t.value[i] + (x3 - 2.0 * x2 + x) * h * t.slope[i]
          + (-2.0 * x3 + 3.0 * x2) * t.value[i + 1] + (x3 - x2) * h * t.slope[i + 1];
        sig = t.slope[i] + x * (t.slope[i + 1] - t.slope[i]);
    }

    /** Reduces the linear part of the circuit for the current sample rate and cathode resistor. */
    void build()
    {
        gcIn = 2.0 * Cin / T; gcK = 2.0 * Ck / T; gcO = 2.0 * Cout / T;

        // nodes: 0 = junction after Cin, 1 = grid, 2 = cathode, 3 = plate, 4 = output
        double G[5][5] {};
        G[0][0] = gcIn + 1.0 / Rgl + 1.0 / Rgs;     G[0][1] = -1.0 / Rgs;
        G[1][0] = -1.0 / Rgs;                       G[1][1] = 1.0 / Rgs;
        G[2][2] = 1.0 / Rk + gcK;
        G[3][3] = 1.0 / Rp + gcO;                   G[3][4] = -gcO;
        G[4][3] = -gcO;                             G[4][4] = 1.0 / Rload + gcO;

        // sources: input voltage, the three capacitor history currents, and the supply (as a constant 1)
        double B[5][5] {};
        B[0][0] = gcIn;  B[0][1] = 1.0;
        B[2][2] = 1.0;
        B[3][3] = 1.0;   B[3][4] = supply / Rp;
        B[4][3] = -1.0;

        // valve currents: plate current leaves the plate and enters the cathode, grid current leaves the grid
        double N[5][2] {};
        N[1][1] = -1.0;
        N[2][0] = 1.0;   N[2][1] = 1.0;
        N[3][0] = -1.0;

        double inv[5][5];
        invert (G, inv);

        double GB[5][5] {}, GN[5][2] {};
        for (int r = 0; r < 5; ++r)
        {
            for (int c = 0; c < 5; ++c) for (int k = 0; k < 5; ++k) GB[r][c] += inv[r][k] * B[k][c];
            for (int c = 0; c < 2; ++c) for (int k = 0; k < 5; ++k) GN[r][c] += inv[r][k] * N[k][c];
        }

        for (int c = 0; c < 5; ++c) { P[0][c] = GB[1][c] - GB[2][c]; P[1][c] = GB[3][c] - GB[2][c]; }
        for (int c = 0; c < 2; ++c) { K[0][c] = GN[1][c] - GN[2][c]; K[1][c] = GN[3][c] - GN[2][c]; }

        // rows: voltage across Cin, across Ck, across Cout, and the output node
        for (int c = 0; c < 5; ++c)
        {
            Mu[0][c] = GB[0][c] - (c == 0 ? 1.0 : 0.0);
            Mu[1][c] = GB[2][c];
            Mu[2][c] = GB[3][c] - GB[4][c];
            Mu[3][c] = GB[4][c];
        }
        for (int c = 0; c < 2; ++c)
        {
            Mi[0][c] = GN[0][c];
            Mi[1][c] = GN[2][c];
            Mi[2][c] = GN[3][c] - GN[4][c];
            Mi[3][c] = GN[4][c];
        }

        // resting point: no signal, capacitors charged, grid at ground. Solve for the plate current by bisection.
        double lo = 0.0, hi = supply / (Rp + Rk);
        for (int i = 0; i < 80; ++i)
        {
            const double mid = 0.5 * (lo + hi);
            double ip, ig, a, b, c;
            currents (-Rk * mid, supply - (Rp + Rk) * mid, ip, ig, a, b, c);
            (ip > mid ? lo : hi) = mid;
        }
        const double i0 = 0.5 * (lo + hi);
        restVk = Rk * i0;
        restVp = supply - Rp * i0;

        double ip, ig, gm, gp, dIg;
        currents (-restVk, restVp - restVk, ip, ig, gm, gp, dIg);
        smallSignalGain = -gm / (gp + 1.0 / Rp + 1.0 / Rload);      // cathode fully bypassed in the audio band
        setInputScale (inScale);
    }

    static void invert (const double a[5][5], double out[5][5]) noexcept
    {
        double m[5][10];
        for (int r = 0; r < 5; ++r)
            for (int c = 0; c < 5; ++c) { m[r][c] = a[r][c]; m[r][c + 5] = r == c ? 1.0 : 0.0; }

        for (int col = 0; col < 5; ++col)
        {
            int best = col;
            for (int r = col + 1; r < 5; ++r) if (std::abs (m[r][col]) > std::abs (m[best][col])) best = r;
            if (best != col) for (int c = 0; c < 10; ++c) std::swap (m[col][c], m[best][c]);

            const double d = 1.0 / m[col][col];
            for (int c = 0; c < 10; ++c) m[col][c] *= d;
            for (int r = 0; r < 5; ++r)
                if (r != col)
                {
                    const double f = m[r][col];
                    if (f != 0.0) for (int c = 0; c < 10; ++c) m[r][c] -= f * m[col][c];
                }
        }
        for (int r = 0; r < 5; ++r) for (int c = 0; c < 5; ++c) out[r][c] = m[r][c + 5];
    }

    /** The last point at which the valve was evaluated, with its currents and slopes, plus the capacitor histories. */
    struct State
    {
        double egk = -1.2, epk = 170.0, ip = 0.0, ig = 0.0, dIpG = 0.0, dIpP = 0.0, dIg = 0.0;
        double hIn = 0.0, hK = 0.0, hO = 0.0;
    };
    static constexpr double stepTolerance = 1.0e-3;

    double T = 1.0 / 192000.0, Rk = 1500.0;
    double gcIn = 0.0, gcK = 0.0, gcO = 0.0;
    double P[2][5] {}, K[2][2] {}, Mu[4][5] {}, Mi[4][2] {};
    double restVk = 1.2, restVp = 170.0, smallSignalGain = -58.0;
    double inScale = 0.2, outScale = 1.0;
    State st[2];
};
} // namespace atelier
