# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Ceiling.io is a **High Performance DSP** audio mastering system built with JUCE. It normalizes audio to streaming platform loudness standards (Spotify, Apple Music, YouTube, Tidal) with genre-specific processing.

## Build Commands

```bash
# Build both CLI and Server
cd build && cmake .. && make -j$(nproc)

# Build specific targets
make ceilingIOCli
make ceilingIOServer

# Clean rebuild
rm -rf build && mkdir build && cd build && cmake .. && make -j$(nproc)
```

The build requires JUCE (found via CMake's find_package). Optional `JUCE_DIR` can be set to a local JUCE source tree.

## Architecture

### Two Executables
- **`ceilingIOCli`** (`src/Cli/MainCli.cpp`) — Standalone CLI for local file processing
- **`ceilingIOServer`** (`src/Server/MainServer.cpp`) — REST API server for cloud job processing

Both link against shared DSP code in `src/Shared/`.

### Server REST API (Crow + Asio)
- `POST /api/v1/master/jobs` — Submit a mastering job
- `GET /api/v1/master/jobs/<jobId>` — Query job status
- `GET /health` — Health check

### Server Pipeline (4 Stages — `MasterJob::runJob()`)
1. **Fetch** — Download audio from presigned R2 URL
2. **Analysis** — LUFS/RMS/Peak analysis via ITU-R BS.1770 K-weighting
3. **Render** — DSP chain (EQ → Compressor → Brickwall Limiter) → 24-bit WAV
4. **Upload** — PUT rendered file to presigned R2 output URL

### Shared DSP (`ceilingIO` namespace)
- `ceilingIOPipeline.cpp` — Platform/genre presets, analysis, render, MP3 encoding (LAME)
- `MainAudioProcessor.cpp` — JUCE AudioProcessor with:
  - 3-band EQ (Low shelf, Mid peak, High shelf)
  - Compressor with ratio/attack/release
  - Brickwall limiter with lookahead using monotonic queues
  - EBU R128 loudness analyzer (integrated LUFS, short-term, momentary, true peak)
- `AnalysisWorker.cpp` — Background thread for offline file analysis

### Platform Presets (target LUFS, max true peak)
`spotify` (-14 LUFS), `apple-music` (-16 LUFS), `youtube` (-14 LUFS), `tidal` (-14 LUFS)

### Genre Presets (EQ curve + compressor settings)
`electronic`, `acoustic`, `pop`, `rock`, `hiphop`

### Thread Safety
- `JobStore` uses `std::shared_mutex` for thread-safe job updates
- `JobRecord` fields updated atomically or under mutex via `updateAndNotify()`
- VU/loudness meters use `std::atomic`

### Output Formats
Server supports WAV (default) and MP3 (via LAME encoder). CLI auto-detects from file extension.
