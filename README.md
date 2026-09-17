# Unofficial Creality OrcaSlicer with CFS, Camera Support

> **Independent community fork.** This is not an OrcaSlicer release and not a Creality product.
> It is not made, endorsed, supported or reviewed by the OrcaSlicer project or by Creality.
> OrcaSlicer, Creality, K2 and CFS are trademarks of their owners and are used only to say what
> this is a fork of and which printers it targets. Do not report problems with this build to the
> OrcaSlicer project or to Creality: use this repository's Issues page. Use it at your own risk.

## What this fork adds

A build of [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) for the Creality K2 family with
CFS units:

- A **Creality CFS print host type**. Send a job to the printer, map each extruder to a real CFS
  slot in the send dialog, and the printer loads the right spools. The slot's temperature range
  is raised to the job's temperature before the print starts, so the printer's own calibration
  and loading run at the right heat.
- A **Device tab** that shows the printer's camera, or, with the companion bridge installed,
  Fluidd with a CFS card for the four units.
- A product name and settings folder of its own (`OrcaSlicerCFS`), so it installs beside an
  official OrcaSlicer without touching it.

Everything else is upstream OrcaSlicer, unchanged. Upstream's own README is kept here as
[UPSTREAM_README.md](UPSTREAM_README.md); upstream's documentation, wiki and community apply to
the slicer itself, not to the additions in this fork.

## Installing

Releases are on this repository's [Releases](https://github.com/jscottdouglas/OrcaSlicer/releases)
page: a Windows x64 installer and a Linux AppImage, with SHA-256 checksums. The companion bridge,
[jscottdouglas/creality-cfs-bridge](https://github.com/jscottdouglas/creality-cfs-bridge), has its
own installer that can download and run this one in the same pass.

The Linux AppImage needs the host packages `libopengl0`, `libglu1-mesa` and `libwebkit2gtk-4.1-0`
and a generated UTF-8 locale; it has only been tried on Ubuntu 24.04.

## Source

Branch `cfs` of this repository, on top of upstream OrcaSlicer commit `3e1daccd7c`. The fork's
additions live in `src/slic3r/Utils/CrealityCFS.cpp` and `.hpp` and the Device tab code that calls
them; `scripts/privacy_scan.py` gates every commit. Licence: AGPL-3.0, as upstream.
