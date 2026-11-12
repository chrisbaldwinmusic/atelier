#pragma once

#include "TubeCircuit.h"

/*  The other two valve stages, solved as circuits in the same way as the triode in TubeCircuit.h.

    Both are built on one small engine. The linear part of a circuit (resistors, and capacitors and
    inductors as trapezoidal companion models) is reduced ahead of time to a few numbers per valve
    voltage; each sample, Newton's method then solves only for the voltages the valves care about.
*/
namespace atelier
{
namespace valve
{
/** Solves G X = R for several right-hand sides at once. X replaces R; G is destroyed. */
template <int N, int M>
inline void solveInPlace (double G[N][N], double R[N][M]) noexcept
{
    for (int col = 0; col < N; ++col)
    {
        int best = col;
        for (int r = col + 1; r < N; ++r) if (std::abs (G[r][col]) > std::abs (G[best][col])) best = r;
        if (best != col)
        {
            for (int c = 0; c < N; ++c) std::swap (G[col][c], G[best][c]);
            for (int c = 0; c < M; ++c) std::swap (R[col][c], R[best][c]);
        }

        const double d = 1.0 / G[col][col];
        for (int c = 0; c < N; ++c) G[col][c] *= d;
        for (int c = 0; c < M; ++c) R[col][c] *= d;

        for (int r = 0; r < N; ++r)
        {
            const double f = G[r][col];
            if (r == col || f == 0.0) continue;
            for (int c = 0; c < N; ++c) G[r][c] -= f * G[col][c];
            for (int c = 0; c < M; ++c) R[r][c] -= f * R[col][c];
        }
    }
}

/** A circuit with NX valve voltages (and as many valve currents), NU sources and NM measured quantities.

    `Device` describes the valves:
      - `Device::currents (point)` fills in the currents and their slopes at point.x;
      - `Device::slope[i][v]` says which current depends on which voltage, so the rest is never multiplied out;
      - `Device::main[2]` names the two currents that always flow, and `Device::quiet (point)` says whether they are
        the only ones flowing (no grid current). That is nearly always so, and then the Newton step only needs a
        2x2 inverse however many voltages there are, because the equations differ from the identity by rank two. */
template <int NX, int NU, int NM, typename Device>
struct Core
{
    /** Where the valves were last evaluated: their voltages, currents and slopes, and the Newton step prepared from them. */
    struct Point { double x[NX] {}, i[NX] {}, d[NX][NX] {}, ki[NX] {}, m[2][2] {}, inv[NX][NX] {}; bool quiet = false; };

    double P[NX][NU] {}, K[NX][NX] {}, Mu[NM][NU] {}, Mi[NM][NX] {};

    /** @param G   conductances between the NN nodes
        @param B   how each source feeds each node
        @param N   how each valve current feeds each node
        @param Sx  each valve voltage as a combination of node voltages
        @param Sm  each measured quantity as a combination of node voltages */
    template <int NN>
    void reduce (double G[NN][NN], const double B[NN][NU], const double N[NN][NX],
                 const double Sx[NX][NN], const double Sm[NM][NN]) noexcept
    {
        double R[NN][NU + NX];
        for (int r = 0; r < NN; ++r)
        {
            for (int c = 0; c < NU; ++c) R[r][c] = B[r][c];
            for (int c = 0; c < NX; ++c) R[r][NU + c] = N[r][c];
        }
        solveInPlace<NN, NU + NX> (G, R);

        auto pick = [&R] (const double* weights, int column)
        {
            double acc = 0.0;
            for (int n = 0; n < NN; ++n) acc += weights[n] * R[n][column];
            return acc;
        };
        for (int r = 0; r < NX; ++r)
        {
            for (int c = 0; c < NU; ++c) P[r][c] = pick (Sx[r], c);
            for (int c = 0; c < NX; ++c) K[r][c] = pick (Sx[r], NU + c);
        }
        for (int r = 0; r < NM; ++r)
        {
            for (int c = 0; c < NU; ++c) Mu[r][c] = pick (Sm[r], c);
            for (int c = 0; c < NX; ++c) Mi[r][c] = pick (Sm[r], NU + c);
        }
    }

    /** Evaluates the valves at point.x and prepares the Newton step from there: the equations are
        F(x) = x - P u - K i(x) = 0, so the step needs K i and the inverse of the Jacobian I - K d. */
    inline void evaluate (Point& s) const noexcept
    {
        Device::currents (s);
        s.quiet = Device::quiet (s);

        if (s.quiet)
        {
            // J = I - U V with U = the two main columns of K and V = the two main rows of d,
            // so J^-1 = I + U (I - V U)^-1 V: only the 2x2 in the middle needs inverting
            const int m0 = Device::main[0], m1 = Device::main[1];
            double vu[2][2] {};
            for (int v = 0; v < NX; ++v)
            {
                if (Device::slope[m0][v]) { vu[0][0] += s.d[m0][v] * K[v][m0]; vu[0][1] += s.d[m0][v] * K[v][m1]; }
                if (Device::slope[m1][v]) { vu[1][0] += s.d[m1][v] * K[v][m0]; vu[1][1] += s.d[m1][v] * K[v][m1]; }
            }
            const double a00 = 1.0 - vu[0][0], a01 = -vu[0][1], a10 = -vu[1][0], a11 = 1.0 - vu[1][1];
            const double det = a00 * a11 - a01 * a10;
            if (std::abs (det) > 1.0e-30)
            {
                const double scale = 1.0 / det;
                s.m[0][0] = a11 * scale;  s.m[0][1] = -a01 * scale;
                s.m[1][0] = -a10 * scale; s.m[1][1] = a00 * scale;
                for (int r = 0; r < NX; ++r) s.ki[r] = K[r][m0] * s.i[m0] + K[r][m1] * s.i[m1];
                return;
            }
            s.quiet = false;
        }

        double a[NX][2 * NX];
        for (int r = 0; r < NX; ++r)
        {
            double ki = 0.0;
            for (int c = 0; c < NX; ++c)
            {
                ki += K[r][c] * s.i[c];
                double j = r == c ? 1.0 : 0.0;
                for (int k = 0; k < NX; ++k) if (Device::slope[k][c]) j -= K[r][k] * s.d[k][c];
                a[r][c] = j;
                a[r][NX + c] = r == c ? 1.0 : 0.0;
            }
            s.ki[r] = ki;
        }

        for (int col = 0; col < NX; ++col)
        {
            int best = col;
            for (int r = col + 1; r < NX; ++r) if (std::abs (a[r][col]) > std::abs (a[best][col])) best = r;
            if (best != col) for (int c = 0; c < 2 * NX; ++c) std::swap (a[col][c], a[best][c]);
            if (! (std::abs (a[col][col]) > 1.0e-30)) return;       // keep the previous inverse

            const double scale = 1.0 / a[col][col];
            for (int c = col; c < 2 * NX; ++c) a[col][c] *= scale;
            for (int r = 0; r < NX; ++r)
            {
                if (r == col) continue;
                const double f = a[r][col];
                for (int c = col; c < 2 * NX; ++c) a[r][c] -= f * a[col][c];
            }
        }
        for (int r = 0; r < NX; ++r) for (int c = 0; c < NX; ++c) s.inv[r][c] = a[r][NX + c];
    }

    /** Solves for the valve voltages, starting from the last point, and gives the valve currents.
        Returns false if the solution went non-finite.

        The first Newton step costs almost nothing: it reuses the step prepared at the last evaluation, which
        lands close enough that one fresh evaluation usually finishes. After a fresh evaluation, a step smaller
        than `tol` leaves an error far below audibility (Newton's error shrinks with the square of the step),
        so the currents are carried across it instead of evaluating again. */
    inline bool solve (Point& s, const double* u, double* current, const double* limit, const double* tol,
                       int maxIterations = 14) const noexcept
    {
        double p[NX], x[NX], dx[NX];
        for (int r = 0; r < NX; ++r)
        {
            double acc = 0.0;
            for (int j = 0; j < NU; ++j) acc += P[r][j] * u[j];
            p[r] = acc; x[r] = s.x[r];
        }

        auto step = [&]
        {
            double f[NX];
            for (int r = 0; r < NX; ++r) f[r] = x[r] - p[r] - s.ki[r];
            double w0 = 0.0, w1 = 0.0;
            const int m0 = Device::main[0], m1 = Device::main[1];
            if (s.quiet)
            {
                double t0 = 0.0, t1 = 0.0;
                for (int v = 0; v < NX; ++v)
                {
                    if (Device::slope[m0][v]) t0 += s.d[m0][v] * f[v];
                    if (Device::slope[m1][v]) t1 += s.d[m1][v] * f[v];
                }
                w0 = s.m[0][0] * t0 + s.m[0][1] * t1;
                w1 = s.m[1][0] * t0 + s.m[1][1] * t1;
            }

            bool small = true;
            for (int r = 0; r < NX; ++r)
            {
                double acc = 0.0;
                if (s.quiet)    acc = f[r] + K[r][m0] * w0 + K[r][m1] * w1;
                else            for (int c = 0; c < NX; ++c) acc += s.inv[r][c] * f[c];
                // keep each step sensible so the exponential in the valve model can't run away
                dx[r] = std::clamp (acc, -limit[r], limit[r]);
                x[r] -= dx[r];
                small = small && std::abs (dx[r]) < tol[r];
            }
            return small;
        };

        step();
        for (int it = 1; it < maxIterations; ++it)
        {
            for (int r = 0; r < NX; ++r) s.x[r] = x[r];
            evaluate (s);
            if (step())
            {
                for (int r = 0; r < NX; ++r)
                {
                    double acc = s.i[r];
                    for (int c = 0; c < NX; ++c) if (Device::slope[r][c]) acc -= s.d[r][c] * dx[c];
                    current[r] = std::max (0.0, acc);
                }
                return true;
            }
        }

        for (int r = 0; r < NX; ++r) if (! std::isfinite (x[r])) return false;
        for (int r = 0; r < NX; ++r) s.x[r] = x[r];
        evaluate (s);
        for (int r = 0; r < NX; ++r) current[r] = s.i[r];
        return true;
    }

    /** The measured quantities, once the valve currents are known. */
    inline void measure (const double* u, const double* current, double* m) const noexcept
    {
        for (int r = 0; r < NM; ++r)
        {
            double acc = 0.0;
            for (int c = 0; c < NX; ++c) acc += Mi[r][c] * current[c];
            for (int j = 0; j < NU; ++j) acc += Mu[r][j] * u[j];
            m[r] = acc;
        }
    }

    /** Finds where the circuit comes to rest with no signal. `steady` is the same circuit with its capacitors open;
        the search starts from `rest`, which is updated, along with the currents and measurements there. */
    void settle (const Core& steady, Point& rest, const double* limit, double* current, double* m) const noexcept
    {
        double u[NU] {}, tight[NX];
        u[NU - 1] = 1.0;
        for (auto& t : tight) t = 1.0e-9;
        steady.evaluate (rest);
        steady.solve (rest, u, current, limit, tight, 200);
        steady.measure (u, current, m);
        evaluate (rest);
    }

    /** Moves a running state from one resting point to another, so re-biasing doesn't thump. */
    void rebias (Point& s, const Point& from, const Point& to) const noexcept
    {
        for (int r = 0; r < NX; ++r) s.x[r] += to.x[r] - from.x[r];
        evaluate (s);
    }
};
} // namespace valve

//==============================================================================
/** A pentode gain stage: an EF86, the small-signal pentode of classic microphone and console preamps.

        in --Cin--+--Rgs-- grid         B+ --Rp--+-- plate --Cout--+-- out       B+ --Rs--+-- screen
                  |                              |                 |                      |
                 Rgl             cathode --+-- Rk               Rload                     Cs
                  |                        +-- Ck                  |                      |
                 gnd                      gnd                     gnd                    gnd

    The screen grid shields the plate, so the plate current hardly depends on the plate voltage until the plate
    swings right down to the "knee": the stage has several times the triode's gain and stays straight over a
    wide swing, then flattens abruptly. The screen has its own supply resistor and capacitor, so sustained loud
    passages pull the screen voltage down and the whole stage eases off, then recovers.

    Device values are the widely circulated Koren-form fit for the EF86. They reproduce the datasheet's
    operating point (3 mA plate, 0.6 mA screen at 250 V, 140 V on the screen, -2 V on the grid). */
struct PentodeCircuit
{
    static constexpr double supply = 250.0, Rp = 220.0e3, Rs = 1.0e6, Cs = 100.0e-9;
    static constexpr double Cin = 47.0e-9, Rgl = 1.0e6, Rgs = 33.0e3, Ck = 22.0e-6, Cout = 100.0e-9, Rload = 1.0e6;
    // EF86
    static constexpr double mu = 34.9, ex = 1.35, kg1 = 2648.1, kg2 = 4627.0, kp = 222.06, kvb = 4.7, gridK = 1300.0;

    void prepare (double sampleRate)
    {
        T = 1.0 / sampleRate;
        valve::softplusTable();
        rest = Point();
        rest.x[0] = -2.0; rest.x[1] = 80.0; rest.x[2] = 70.0;
        for (auto& h : restH) h = 0.0;
        Rk = -1.0;
        setCathodeResistor (2200.0);
        reset();
    }

    /** Puts every capacitor at its resting voltage, as if the stage had been on for a while. */
    void reset() noexcept
    {
        for (auto& s : st) { s.point = rest; for (int k = 0; k < 4; ++k) s.h[k] = restH[k]; }
    }

    /** The cathode resistor sets the operating point: small = hot, large = cold and closer to cut-off. */
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
    double restingPlate() const noexcept    { return rest.x[1] + restK; }
    double restingScreen() const noexcept   { return rest.x[2] + restK; }
    double restingCathode() const noexcept  { return restK; }

    inline float process (int ch, float x) noexcept
    {
        return (float) (processVolts (ch, (double) x * inScale) * outScale);
    }

    /** One sample, in volts at the input and at the loaded output. */
    inline double processVolts (int ch, double vin) noexcept
    {
        auto& s = st[ch];
        const double u[6] = { vin, s.h[0], s.h[1], s.h[2], s.h[3], 1.0 };
        double current[3], m[5];
        if (! core.solve (s.point, u, current, stepLimit, stepTolerance)) { reset(); return 0.0; }
        core.measure (u, current, m);

        s.h[0] = 2.0 * gcIn * m[0] - s.h[0];
        s.h[1] = 2.0 * gcK  * m[1] - s.h[1];
        s.h[2] = 2.0 * gcO  * m[2] - s.h[2];
        s.h[3] = 2.0 * gcS  * m[3] - s.h[3];
        return m[4];
    }

private:
    /** Koren pentode. Voltages: grid, plate and screen, each against the cathode. Currents: plate, grid, screen. */
    struct Device
    {
        static constexpr bool slope[3][3] = { { true, true, true }, { true, false, false }, { true, false, true } };
        static constexpr int main[2] = { 0, 2 };
        template <typename Point> static inline bool quiet (const Point& s) noexcept   { return s.i[1] == 0.0 && s.d[1][0] == 0.0; }

        template <typename Point>
        static inline void currents (Point& s) noexcept
        {
            // constants as reciprocals: a multiply is several times quicker than a divide, and this runs every sample
            constexpr double rMu = 1.0 / mu, rKp = 1.0 / kp, rKvb = 1.0 / kvb, rKg1 = 2.0 / kg1, rKg2 = 1.0 / kg2, rGrid = 1.0 / gridK;

            const double vg = s.x[0], vp = s.x[1], vs = std::max (s.x[2], 1.0), rVs = 1.0 / vs;
            const bool screenLive = s.x[2] > 1.0;

            double L, sig;
            valve::softplus (kp * (rMu + vg * rVs), L, sig);
            const double e1 = vs * rKp * L;

            if (e1 > 0.0 && vp > 0.0)
            {
                // single precision is ample for the power and the arctangent
                const double ratio = vp * rKvb, knee = (double) std::atan ((float) ratio);
                const double pw = rKg1 * (double) std::pow ((float) e1, (float) (ex - 1.0));
                const double dE = ex * pw * knee;
                s.i[0] = pw * e1 * knee;
                s.d[0][0] = dE * sig;
                s.d[0][1] = pw * e1 * rKvb / (1.0 + ratio * ratio);
                s.d[0][2] = screenLive ? dE * (L * rKp - sig * vg * rVs) : 0.0;
            }
            else { s.i[0] = s.d[0][0] = s.d[0][1] = s.d[0][2] = 0.0; }

            if (vg > 0.0)   { const double r = std::sqrt (vg); s.i[1] = vg * r * rGrid; s.d[1][0] = 1.5 * r * rGrid; }
            else            { s.i[1] = s.d[1][0] = 0.0; }

            const double q = vg + vs * rMu;
            if (q > 0.0)
            {
                const double pw = rKg2 * (double) std::pow ((float) q, (float) (ex - 1.0));
                s.i[2] = pw * q;
                s.d[2][0] = ex * pw;
                s.d[2][2] = screenLive ? ex * pw * rMu : 0.0;
            }
            else { s.i[2] = s.d[2][0] = s.d[2][2] = 0.0; }
        }
    };

    using Core = valve::Core<3, 6, 5, Device>;
    using Point = Core::Point;

    /** The circuit's equations, either for a sample period or (steady = true) with every capacitor open. */
    void assemble (Core& c, bool steady) const noexcept
    {
        const double cIn = steady ? 0.0 : gcIn, cK = steady ? 0.0 : gcK, cO = steady ? 0.0 : gcO, cS = steady ? 0.0 : gcS;

        // nodes: 0 = junction after Cin, 1 = grid, 2 = cathode, 3 = plate, 4 = output, 5 = screen
        double G[6][6] {};
        G[0][0] = cIn + 1.0 / Rgl + 1.0 / Rgs;      G[0][1] = -1.0 / Rgs;
        G[1][0] = -1.0 / Rgs;                       G[1][1] = 1.0 / Rgs;
        G[2][2] = 1.0 / Rk + cK;
        G[3][3] = 1.0 / Rp + cO;                    G[3][4] = -cO;
        G[4][3] = -cO;                              G[4][4] = 1.0 / Rload + cO;
        G[5][5] = 1.0 / Rs + cS;

        // sources: input voltage, the four capacitor histories (in, cathode, out, screen), and the supply (as a constant 1)
        double B[6][6] {};
        B[0][0] = cIn;   B[0][1] = 1.0;
        B[2][2] = 1.0;
        B[3][3] = 1.0;   B[3][5] = supply / Rp;
        B[4][3] = -1.0;
        B[5][4] = 1.0;   B[5][5] = supply / Rs;

        // valve currents: plate and screen current enter the valve there and leave by the cathode; grid current leaves the grid
        double N[6][3] {};
        N[1][1] = -1.0;
        N[2][0] = 1.0;   N[2][1] = 1.0;   N[2][2] = 1.0;
        N[3][0] = -1.0;
        N[5][2] = -1.0;

        double Sx[3][6] {}, Sm[5][6] {};
        Sx[0][1] = 1.0;  Sx[0][2] = -1.0;
        Sx[1][3] = 1.0;  Sx[1][2] = -1.0;
        Sx[2][5] = 1.0;  Sx[2][2] = -1.0;

        // measured: voltage across Cin, across Ck, across Cout, across Cs, and the output
        Sm[0][0] = 1.0;
        Sm[1][2] = 1.0;
        Sm[2][3] = 1.0;  Sm[2][4] = -1.0;
        Sm[3][5] = 1.0;
        Sm[4][4] = 1.0;

        c.reduce<6> (G, B, N, Sx, Sm);
        c.Mu[0][0] -= 1.0;      // Cin sits between the input and the junction
    }

    void build()
    {
        gcIn = 2.0 * Cin / T; gcK = 2.0 * Ck / T; gcO = 2.0 * Cout / T; gcS = 2.0 * Cs / T;
        assemble (core, false);

        // Resting point: no signal, every capacitor charged. The search starts from the previous one, so a moving
        // Bias knob is cheap, and whatever is playing is carried across to the new operating point.
        Core steady;
        assemble (steady, true);
        const Point before = rest;
        double current[3], m[5];
        core.settle (steady, rest, stepLimit, current, m);
        restK = m[1];
        const double h[4] = { gcIn * m[0], gcK * m[1], gcO * m[2], gcS * m[3] };
        for (auto& s : st)
        {
            for (int k = 0; k < 4; ++k) s.h[k] += h[k] - restH[k];
            core.rebias (s.point, before, rest);
        }
        for (int k = 0; k < 4; ++k) restH[k] = h[k];

        // cathode and screen are both bypassed in the audio band
        smallSignalGain = -rest.d[0][0] / (rest.d[0][1] + 1.0 / Rp + 1.0 / Rload);
        setInputScale (inScale);
    }

    struct State { Point point; double h[4] {}; };
    static constexpr double stepLimit[3] = { 1.5, 60.0, 60.0 }, stepTolerance[3] = { 1.0e-3, 5.0e-2, 5.0e-2 };

    Core core;
    Point rest;
    double T = 1.0 / 192000.0, Rk = 2200.0;
    double gcIn = 0.0, gcK = 0.0, gcO = 0.0, gcS = 0.0;
    double restK = 1.5, restH[4] {}, smallSignalGain = -150.0;
    double inScale = 0.05, outScale = 1.0;
    State st[2];
};

//==============================================================================
/** A push-pull pair: two 12AU7 triodes working against each other into a centre-tapped output transformer.

        in --Cin--+--Rgs-- grid A      plate A ---+
                  |                               3||
                 Rgl                    B+ -------3||---- out
                  |                               3||
        -in -Cin--+--Rgs-- grid B      plate B ---+
                  |
                 Rgl          both cathodes --+-- Rk
                                              +-- Ck

    The two valves are driven in opposite directions and the transformer takes the difference between their
    currents, so the lopsidedness of each valve cancels against the other: what is left is symmetric, mostly
    3rd harmonic, and cleaner than a single valve at the same level. They share one cathode resistor, so when
    both are driven hard their combined current raises the cathode and the pair biases itself colder for a
    while. Run cold, each valve switches off for part of the cycle and the hand-over between them adds edge.

    No two valves are identical: the pair here is matched to 5 %, which leaves a trace of 2nd harmonic.
    The transformer is ideal apart from its inductance, which keeps DC out of the output. Its saturation
    belongs to the Iron stage. */
struct PushPullCircuit
{
    static constexpr double supply = 250.0, Rh = 5.0e3, Lh = 40.0;      // load and inductance seen by half the primary
    static constexpr double Cin = 47.0e-9, Rgl = 470.0e3, Rgs = 47.0e3, Ck = 100.0e-6;    // a 47k grid stopper keeps grid current in check when slammed
    static constexpr double mismatch = 1.05;

    void prepare (double sampleRate)
    {
        T = 1.0 / sampleRate;
        valve::softplusTable();
        rest = Point();
        rest.x[0] = rest.x[2] = -8.0; rest.x[1] = rest.x[3] = supply - 8.0;
        for (auto& h : restH) h = 0.0;
        Rk = -1.0;
        setCathodeResistor (820.0);
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : st) { s.point = rest; for (int k = 0; k < 4; ++k) s.h[k] = restH[k]; }
    }

    /** The shared cathode resistor: small = both valves conducting all the time, large = colder, handing over between them. */
    void setCathodeResistor (double ohms)
    {
        if (std::abs (ohms - Rk) < 0.5) return;
        Rk = ohms;
        build();
    }

    /** Volts at each grid for a full-scale sample. */
    void setInputScale (double voltsPerUnit) noexcept
    {
        inScale = voltsPerUnit;
        outScale = 1.0 / (smallSignalGain * voltsPerUnit);
    }

    double gain() const noexcept            { return smallSignalGain; }
    double restingCathode() const noexcept  { return restK; }

    inline float process (int ch, float x) noexcept
    {
        return (float) (processVolts (ch, (double) x * inScale) * outScale);
    }

    /** One sample: volts at grid A (grid B gets the opposite), and volts across half the primary. */
    inline double processVolts (int ch, double vin) noexcept
    {
        auto& s = st[ch];
        const double u[6] = { vin, s.h[0], s.h[1], s.h[2], s.h[3], 1.0 };
        double current[4], m[4];
        if (! core.solve (s.point, u, current, stepLimit, stepTolerance)) { reset(); return 0.0; }
        core.measure (u, current, m);

        s.h[0] = 2.0 * gcIn * m[0] - s.h[0];
        s.h[1] = 2.0 * gcIn * m[1] - s.h[1];
        s.h[2] = 2.0 * gcK  * m[2] - s.h[2];
        s.h[3] += 2.0 * gL * m[3];
        return m[3];
    }

private:
    /** Voltages: grid A, plate A, grid B, plate B, each against the shared cathode. Currents: plate A, grid A, plate B, grid B. */
    struct Device
    {
        static constexpr bool slope[4][4] = { { true, true, false, false }, { true, false, false, false },
                                              { false, false, true, true }, { false, false, true, false } };
        static constexpr int main[2] = { 0, 2 };
        template <typename Point> static inline bool quiet (const Point& s) noexcept   { return s.i[1] == 0.0 && s.i[3] == 0.0 && s.d[1][0] == 0.0 && s.d[3][2] == 0.0; }
        static constexpr valve::Triode a = valve::ecc82;
        static constexpr valve::Triode b { valve::ecc82.mu, valve::ecc82.ex, valve::ecc82.kg1 * mismatch,
                                           valve::ecc82.kp, valve::ecc82.kvb, valve::ecc82.gridK };

        template <typename Point>
        static inline void currents (Point& s) noexcept
        {
            a.currents (s.x[0], s.x[1], s.i[0], s.i[1], s.d[0][0], s.d[0][1], s.d[1][0]);
            b.currents (s.x[2], s.x[3], s.i[2], s.i[3], s.d[2][2], s.d[2][3], s.d[3][2]);
        }
    };

    using Core = valve::Core<4, 6, 4, Device>;
    using Point = Core::Point;

    void assemble (Core& c, bool steady) const noexcept
    {
        const double cIn = steady ? 0.0 : gcIn, cK = steady ? 0.0 : gcK;
        const double gl = steady ? 1.0e3 : gL;          // at rest the winding is a short circuit

        // nodes: 0 = junction A, 1 = grid A, 2 = junction B, 3 = grid B, 4 = cathodes, 5 = voltage across half the primary
        double G[6][6] {};
        for (int n : { 0, 2 })
        {
            G[n][n] = cIn + 1.0 / Rgl + 1.0 / Rgs;      G[n][n + 1] = -1.0 / Rgs;
            G[n + 1][n] = -1.0 / Rgs;                   G[n + 1][n + 1] = 1.0 / Rgs;
        }
        G[4][4] = 1.0 / Rk + cK;
        G[5][5] = 1.0 / Rh + gl;

        // sources: input voltage, histories of the two input capacitors, the cathode capacitor and the winding, and a constant 1
        double B[6][6] {};
        B[0][0] = cIn;   B[0][1] = 1.0;
        B[2][0] = -cIn;  B[2][2] = 1.0;
        B[4][3] = 1.0;
        B[5][4] = -1.0;

        // valve currents: both plates' and grids' currents arrive at the cathode; the winding sees the difference of the plates'
        double N[6][4] {};
        N[1][1] = -1.0;  N[3][3] = -1.0;
        N[4][0] = N[4][1] = N[4][2] = N[4][3] = 1.0;
        N[5][0] = 1.0;   N[5][2] = -1.0;

        // plate A sits at supply - w, plate B at supply + w
        double Sx[4][6] {}, Sm[4][6] {};
        Sx[0][1] = 1.0;   Sx[0][4] = -1.0;
        Sx[1][5] = -1.0;  Sx[1][4] = -1.0;
        Sx[2][3] = 1.0;   Sx[2][4] = -1.0;
        Sx[3][5] = 1.0;   Sx[3][4] = -1.0;

        // measured: voltage across each input capacitor, across Ck, and across half the primary (the output)
        Sm[0][0] = 1.0;
        Sm[1][2] = 1.0;
        Sm[2][4] = 1.0;
        Sm[3][5] = 1.0;

        c.reduce<6> (G, B, N, Sx, Sm);
        c.P[1][5] += supply;  c.P[3][5] += supply;
        c.Mu[0][0] -= 1.0;    c.Mu[1][0] += 1.0;
    }

    void build()
    {
        gcIn = 2.0 * Cin / T; gcK = 2.0 * Ck / T; gL = T / (2.0 * Lh);
        assemble (core, false);

        Core steady;
        assemble (steady, true);
        const Point before = rest;
        double current[4], m[4];
        core.settle (steady, rest, stepLimit, current, m);
        restK = m[2];
        // the standing imbalance between the two valves flows in the winding
        const double h[4] = { gcIn * m[0], gcIn * m[1], gcK * m[2], current[0] - current[2] };
        for (auto& s : st)
        {
            for (int k = 0; k < 4; ++k) s.h[k] += h[k] - restH[k];
            core.rebias (s.point, before, rest);
        }
        for (int k = 0; k < 4; ++k) restH[k] = h[k];

        // in the audio band the cathode is held still and the winding's inductance is out of the picture
        smallSignalGain = Rh * (rest.d[0][0] + rest.d[2][2]) / (1.0 + Rh * (rest.d[0][1] + rest.d[2][3]));
        setInputScale (inScale);
    }

    struct State { Point point; double h[4] {}; };
    static constexpr double stepLimit[4] = { 15.0, 60.0, 15.0, 60.0 }, stepTolerance[4] = { 1.0e-2, 2.5e-1, 1.0e-2, 2.5e-1 };

    Core core;
    Point rest;
    double T = 1.0 / 192000.0, Rk = 820.0;
    double gcIn = 0.0, gcK = 0.0, gL = 0.0;
    double restK = 8.0, restH[4] {}, smallSignalGain = 10.0;
    double inScale = 1.0, outScale = 1.0;
    State st[2];
};
//==============================================================================
/** The gain element of a variable-mu compressor: two remote-cutoff triodes in push-pull.

        in --+-- Rg -- grid A      plate A ---+
             |                                3||
          (bias + control)          B+ ------ 3||---- out
             |                                3||
       -in --+-- Rg -- grid B      plate B ---+
                                  both cathodes to ground

    A remote-cutoff valve has a grid wound with a varying pitch, so instead of switching off sharply its gain
    falls smoothly as the grid is biased more negative: roughly a dB per volt, gently at first. The compressor's
    side chain turns the level it hears into a negative voltage on both grids, and the valves themselves do the
    turning down. Nothing in between is a perfect multiplier, so the character comes from the valves: the gain
    reduction eases in with a soft knee, and the harmonics grow as the valves are pushed down their curves.

    The control voltage reaches both grids together and so cancels at the output transformer, as it must, or
    every change of gain would be heard as a thump. The two valves are matched to 0.5 %, as a unit trimmed for
    balance on the bench is, so a trace of it is left (a low, brief bump when the gain moves fast), along with a
    little 2nd harmonic.

    Koren's model describes sharp-cutoff valves, so the remote-cutoff valve is made, in the usual way, of several
    sections of different mu sharing one grid and plate. The section values are this project's own fit to a
    smooth fall of gain with bias of the kind remote-cutoff valves such as the 6386 show; they are not from a
    datasheet. */
struct VariMuCircuit
{
    static constexpr double supply = 180.0, restBias = 3.0, Rg = 10.0e3, Rh = 2.0e3, Lh = 100.0, mismatch = 1.005;
    static constexpr int sections = 4;
    static constexpr double sectionMu[sections]  = { 50.0, 20.0, 8.0, 3.2 };
    static constexpr double sectionKg1[sections] = { 113200.1, 5741.7, 10961.8, 126621.3 };

    void prepare (double sampleRate)
    {
        T = 1.0 / sampleRate;
        valve::softplusTable();
        gL = T / (2.0 * Lh);
        assemble (core, false);

        // resting point, then the small-signal gain at each control voltage, which the side chain and the meter use
        Core steady;
        assemble (steady, true);
        rest = Point();
        rest.x[0] = rest.x[2] = -restBias; rest.x[1] = rest.x[3] = supply;
        double current[4], m[1];
        steady.settle (steady, rest, stepLimit, current, m);
        core.evaluate (rest);
        restH = current[0] - current[2];

        for (int k = 0; k <= tableSize; ++k)
        {
            Point p = rest;
            const double vc = maxControl * k / tableSize;
            p.x[0] = p.x[2] = -restBias - vc;
            Device::currents (p);
            const double g = Rh * (p.d[0][0] + p.d[2][2]) / (1.0 + Rh * (p.d[0][1] + p.d[2][3]));
            if (k == 0) restGain = g;
            reductionTable[k] = -20.0 * std::log10 (g / restGain);
        }
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : st) { s.point = rest; s.h = restH; }
    }

    /** Volts at each grid for a full-scale sample. */
    void setInputScale (double voltsPerUnit) noexcept   { inScale = voltsPerUnit; outScale = 1.0 / (restGain * voltsPerUnit); }

    /** Gain reduction (dB, small signal) for a control voltage, and the control voltage that gives a gain reduction. */
    double reduction (double control) const noexcept
    {
        const double pos = std::clamp (control / maxControl, 0.0, 1.0) * tableSize;
        const int i = std::min ((int) pos, tableSize - 1);
        return reductionTable[i] + (pos - i) * (reductionTable[i + 1] - reductionTable[i]);
    }

    double controlFor (double reductionDb) const noexcept
    {
        int lo = 0, hi = tableSize;
        if (reductionDb >= reductionTable[hi]) return maxControl;
        while (hi - lo > 1) { const int mid = (lo + hi) / 2; (reductionTable[mid] < reductionDb ? lo : hi) = mid; }
        const double span = reductionTable[hi] - reductionTable[lo];
        return maxControl * (lo + (span > 0.0 ? (reductionDb - reductionTable[lo]) / span : 0.0)) / tableSize;
    }

    /** One sample; `control` is the side chain's voltage, applied negative to both grids. Unity gain at rest. */
    inline float process (int ch, float x, double control) noexcept
    {
        return (float) (processVolts (ch, (double) x * inScale, control) * outScale);
    }

    /** One sample: volts at grid A (grid B gets the opposite), and volts across half the output winding. */
    inline double processVolts (int ch, double vin, double control) noexcept
    {
        auto& s = st[ch];
        const double u[4] = { vin, control, s.h, 1.0 };
        double current[4], m[1];
        if (! core.solve (s.point, u, current, stepLimit, stepTolerance)) { reset(); return 0.0; }
        core.measure (u, current, m);
        s.h += 2.0 * gL * m[0];
        return m[0];
    }

private:
    struct Device
    {
        static constexpr bool slope[4][4] = { { true, true, false, false }, { true, false, false, false },
                                              { false, false, true, true }, { false, false, true, false } };
        static constexpr int main[2] = { 0, 2 };
        template <typename Point> static inline bool quiet (const Point& s) noexcept   { return s.i[1] == 0.0 && s.i[3] == 0.0 && s.d[1][0] == 0.0 && s.d[3][2] == 0.0; }

        /** One remote-cutoff valve: the sum of its sections. `strength` scales it, for the mismatch between the pair. */
        static inline void valveCurrents (double vg, double vp, double strength, double& ip, double& ig, double& dG, double& dP, double& dIg) noexcept
        {
            ip = dG = dP = 0.0;
            for (int k = 0; k < sections; ++k)
            {
                const valve::Triode t { sectionMu[k], 1.4, sectionKg1[k] / strength, sectionMu[k] * 6.0, 300.0, 1300.0 };
                double i, g, dg, dp, dig;
                t.currents (vg, vp, i, g, dg, dp, dig);
                ip += i; dG += dg; dP += dp;
            }
            if (vg > 0.0) { const double r = std::sqrt (vg); ig = vg * r / 1300.0; dIg = 1.5 * r / 1300.0; }
            else          { ig = dIg = 0.0; }
        }

        template <typename Point>
        static inline void currents (Point& s) noexcept
        {
            valveCurrents (s.x[0], s.x[1], 1.0, s.i[0], s.i[1], s.d[0][0], s.d[0][1], s.d[1][0]);
            valveCurrents (s.x[2], s.x[3], 1.0 / mismatch, s.i[2], s.i[3], s.d[2][2], s.d[2][3], s.d[3][2]);
        }
    };

    using Core = valve::Core<4, 4, 1, Device>;
    using Point = Core::Point;

    void assemble (Core& c, bool steady) const noexcept
    {
        const double gl = steady ? 1.0e3 : gL;      // at rest the winding is a short circuit

        // nodes: 0 = grid A, 1 = grid B, 2 = voltage across half the output winding
        double G[3][3] {};
        G[0][0] = 1.0 / Rg;  G[1][1] = 1.0 / Rg;  G[2][2] = 1.0 / Rh + gl;

        // sources: input voltage, control voltage, the winding's history, and a constant 1
        double B[3][4] {};
        B[0][0] = 1.0 / Rg;   B[0][1] = -1.0 / Rg;  B[0][3] = -restBias / Rg;
        B[1][0] = -1.0 / Rg;  B[1][1] = -1.0 / Rg;  B[1][3] = -restBias / Rg;
        B[2][2] = -1.0;

        double N[3][4] {};
        N[0][1] = -1.0;  N[1][3] = -1.0;
        N[2][0] = 1.0;   N[2][2] = -1.0;

        // plate A sits at the supply minus the winding voltage, plate B at the supply plus it
        double Sx[4][3] {}, Sm[1][3] {};
        Sx[0][0] = 1.0;  Sx[1][2] = -1.0;  Sx[2][1] = 1.0;  Sx[3][2] = 1.0;
        Sm[0][2] = 1.0;

        c.reduce<3> (G, B, N, Sx, Sm);
        c.P[1][3] += supply;  c.P[3][3] += supply;
    }

    struct State { Point point; double h = 0.0; };
    static constexpr int tableSize = 256;
    static constexpr double maxControl = 40.0;
    static constexpr double stepLimit[4] = { 15.0, 60.0, 15.0, 60.0 }, stepTolerance[4] = { 1.0e-2, 2.5e-1, 1.0e-2, 2.5e-1 };

    Core core;
    Point rest;
    double T = 1.0 / 48000.0, gL = 0.0, restH = 0.0, restGain = 1.0;
    double reductionTable[tableSize + 1] {};
    double inScale = 1.0, outScale = 1.0;
    State st[2];
};
} // namespace atelier
