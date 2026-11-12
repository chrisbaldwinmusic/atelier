# Atelier

A boutique channel strip plugin for mixing, bussing and mastering. AU, VST3 and Standalone, built with JUCE.

One strip, three calibrations:

- **Mix** — filters, gate, tube preamp, de-esser, EQ, compressor, tape, output iron.
- **Bus** — the same colour and dynamics, calibrated cleaner, plus stereo image and a limiter.
- **Master** — cleanest calibration, mid/side EQ, true-peak limiter and loudness metering.

## What's in it

- **Tube** — a 12AX7 triode stage, an EF86 pentode stage and a 12AU7 push-pull pair, each solved as a real circuit sample by sample and checked against ngspice; Bias sets the cathode resistor (4x oversampled).
- **Tape** — Jiles-Atherton magnetic hysteresis with pre/de-emphasis, head bump and three speeds (8x oversampled).
- **Iron** — steel, nickel and console output transformers that saturate on low-frequency flux first.
- **Compressor** — four designs that differ in topology, not just timing: VCA, FET, Opto and Vari-Mu. The Vari-Mu turns the gain down with a real push-pull pair of remote-cutoff valves, solved as a circuit and checked against ngspice. External sidechain key, sidechain high-pass, parallel mix.
- **EQ** — five bands, Modern or Vintage curves, mid/side on busses and masters, pre or post compressor.
- **Gate and de-esser** on channels; **stereo image, look-ahead true-peak limiter with a soft clipper ahead of it, and dither** (24 bit, 16 bit, 16 bit shaped) on busses and masters.
- **Analyser** — input and output spectra with a draggable EQ curve, integrated loudness, loudness range, true peak and correlation.
- **Guide** — explains whichever control is under the pointer, gives tips per instrument, bus and master style, comments live on what the strip is doing, and applies starting points fitted to the level that is playing.
- **Workflow** — click-free switching of every module, low-latency tracking mode, level-matched bypass, delta listen, A/B, undo/redo, presets.

## Building (macOS)

Needs the Xcode Command Line Tools, plus CMake and Ninja:

```sh
pip3 install --user cmake ninja
./build.sh
```

`build.sh` fetches JUCE if it is missing, runs the offline test suite, and installs the AU and VST3 into `~/Library/Audio/Plug-Ins`. The build directory is `~/Library/Caches/Atelier-build`.

After installing, quit your DAW, run `killall -9 AudioComponentRegistrar`, and reopen it. The plugin appears under the manufacturer "Channel Audio".

## Tests

`Tests/Main.cpp` runs audio through the processor offline and checks latency and bypass nulling, harmonic and aliasing levels, compressor and limiter behaviour, true-peak and loudness metering, sidechain keying, click-free switching, starting points and CPU use, then renders screenshots of the interface.

If ngspice is installed (at `~/Library/Caches/ngspice-44.2`, or wherever `NGSPICE` points), the valve circuits are also simulated in ngspice and compared sample for sample; otherwise that check is skipped.

## Status

Validated with Apple's `auval`. Pro Tools (AAX) is not supported yet.

Built on [JUCE](https://juce.com), which has its own licence terms (AGPLv3 or commercial).
