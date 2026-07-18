# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Ceiling.io is a C++20 DSP audio mastering pipeline. It analyzes an audio file (LUFS/RMS/true-peak, EBU R128 / ITU-R BS.1770 K-weighting), applies a mastering DSP chain tuned to a streaming-platform target and genre profile, and renders output (WAV or MP3). The same DSP core drives two targets:

- `ceilingIOCli` — local command-line mastering tool
- `ceilingIOServer` — HTTP mastering service (Crow + Asio) that fetches/uploads via presigned R2 URLs

## Build

CMake + JUCE. JUCE is **not vendored** — supply it via `-DJUCE_DIR=/path/to/JUCE` (pointing at a JUCE source tree) or let `find_package(JUCE CONFIG REQUIRED)` resolve a system install. The Dockerfiles `git clone` JUCE and build/install it.

```bash
# Configure + build (out-of-source)
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DJUCE_DIR=/path/to/JUCE
cmake --build . -- -j4

# Single target from repo root
cmake --build build --target ceilingIOServer -- -j4
cmake --build build --target ceilingIOCli    -- -j4
```

Server deps (Crow, Asio) are pulled via `FetchContent` in `CMakeLists.txt`. The `-DceilingIO_FETCH_SERVER_DEPS=ON` flag appears in Dockerfiles and `AGENTS.md` but is **not actually gated** in `CMakeLists.txt` — FetchContent always runs. Don't assume it toggles anything.

Build artifacts land in `build/<target>_artefacts/<BuildType>/<target>`.

### Runtime dependency
`libmp3lame` (and `libmp3lame-dev` for the header) is required — MP3 output is encoded via LAME (`<lame/lame.h>`, linked as `mp3lame`). The cloud/dev Dockerfiles install `libmp3lame-dev` at build time.

### Running

```bash
# Server (port is the first argv, default 8080)
./build/ceilingIOServer_artefacts/Release/ceilingIOServer 8080

# CLI — list targets, analyze, or master to a file
./build/ceilingIOCli_artefacts/Release/ceilingIOCli --list-presets
./build/ceilingIOCli_artefacts/Release/ceilingIOCli -i input.wav -o out.mp3 --preset spotify --genre pop
```

There is no test suite in this repo.

## Architecture

### Shared DSP core (`src/Shared`, `include/Shared`)

- **`MainAudioProcessor`** — a `juce::AudioProcessor` owning the real-time DSP chain: high-pass filters (`hp1`/`hp2`), EQ shelves (`eqLow`/`eqMid`/`eqHigh`), a `juce::dsp::Compressor`, and a custom lookahead limiter (per-channel delay buffers + monotonic-queue peak detection). Parameters are exposed through `juce::AudioProcessorValueTreeState` (`apvts`). It also contains a `LoudnessAnalyzer` (EBU R128 integrated/short-term/momentary LUFS, loudness range, true-peak via oversampling) and VU meter atomics. Despite being an `AudioProcessor`, it is used **offline** here — the server/CLI construct one, configure it, and push audio through it block-by-block.
- **`ceilingIOPipeline`** (namespace `ceilingIO`) — the orchestration layer: analysis (`analyseReader`/`analyseFile`), preset resolution (`findPlatformPreset`/`findGenrePreset`), gain computation (`computeNormalizationGainDb`), processor configuration (`applyAnalysisToProcessor`), WAV→MP3 (`encodeWavToMp3`), and the offline render pass (`renderReaderToMemory`/`renderFile`). Holds the platform and genre preset tables.
- **`AnalysisWorker`** — background-thread analysis wrapper used by the processor's `startFileAnalysis`.

### Server (`src/Server`, `include/Server`)

The server is a Crow HTTP app (`MainServer.cpp`) running on a `juce::ThreadPool`. It accepts mastering jobs, runs them as `MasterJob`s, tracks them in `JobStore`, and notifies callers via `WebhookDispatcher`.

- **`MainServer`** — defines routes, owns `ServerContext` (WebhookDispatcher + JobStore + ThreadPool). Routes: `POST /api/v1/master/jobs` (submit → 202 + `jobId`), `GET /api/v1/master/jobs/<jobId>` (status), `GET /health`. Logs to `ceilingIO_server.log` in the CWD.
- **`JobModels`** — request/response structs, status constants, JSON parsing (`parseSubmitRequest`) with field-length guards (`maxFieldLength = 2048`) and payload signature for idempotency.
- **`JobStore`** — thread-safe (`std::shared_mutex`) in-memory job map + idempotency index. `submit()` dedupes by `idempotencyKey` + payload signature; `updateAndNotify()` mutates a record under lock and fires a webhook.
- **`MasterJob`** (a `juce::ThreadPoolJob`) — the per-job pipeline. Stages, each updating progress and notifying: **fetch** (download from presigned `inputUrl`, ≤100 MB) → **analysis** (decode, LUFS/RMS/peak, resolve presets, compute `renderGainDb` with a low-bump headroom pad) → **render** (DSP chain → 24-bit WAV in memory) → optional **transcode** to MP3 → **upload** (PUT to presigned `outputUrl`). Failures stamp the job `FAILED` with an error code (e.g. `R2_DOWNLOAD_FAILED`, `AUDIO_READ_FAILED`, `DSP_RENDER_FAILED`, `R2_UPLOAD_FAILED`, `MP3_ENCODE_FAILED`).
- **`WebhookDispatcher`** — POSTs job-status JSON to the request's `callbackUrl` from its own thread pool.

### CLI (`src/Cli/MainCli.cpp`)

Standalone driver that mirrors the server's analysis → render → transcode flow against local files. Supports `--preset`, `--genre`, `--target` (custom LUFS), `--intensity` (1/2/3), `--list-presets`. Falls back to a custom-target platform preset when no `--preset` is given (same fallback logic as the server).

### Presets

- **Platform** (`PlatformPreset`): `spotify` (−14 LUFS), `apple-music`/`applemusic` (−16), `youtube` (−14), `tidal` (−14); max true-peak −1.0 dBTP.
- **Genre** (`GenrePreset`): `electronic`, `acoustic`, `pop`, `rock`, `hiphop` — each sets low/mid/high shelf dB plus compressor ratio/attack/release.

Both CLI and server resolve presets by lowercased, trimmed name and use the same `renderGainDb = (targetLufs − inputLufs) + dynamicHeadroomPad` formula (clamped ≥ 0), where `dynamicHeadroomPad = -(genre.baseLowShelfDb * 0.5)` only when the genre's low shelf boosts. **Keep the CLI and server stage math in sync** — they intentionally duplicate this logic.

## Conventions

- **Headers**: `#include <JuceHeader.h>` (generated by `juce_generate_juce_header` per target). Server files additionally `#include <crow.h>`.
- **Adding a source file**: add it to the right target's `target_sources` in `CMakeLists.txt` (shared DSP goes in `ceilingIO_SHARED_SOURCES`, used by both targets). Shared headers live under `include/Shared`; server-only headers under `include/Server`.
- Server uses `juce::String`/`juce::JSON`/`juce::MemoryBlock` throughout; only the HTTP layer touches `crow::json::wvalue`.
- The CLI and server deliberately reimplement the same analysis→gain→render pipeline against different I/O (files vs presigned URLs). Changes to the mastering math in one should be mirrored in the other.

## Docker

Multiple variants exist; the root `Dockerfile` is **stale** — it builds a target named `LocalMixeaServer` and references `localmixea`, which no longer matches this repo's `ceilingIOServer` target. Prefer the layered setup:

- `Dockerfile.base` builds + installs JUCE to `/opt/juce-install`, producing `ceilingio/base:build` and `ceilingio/base:runtime` (the cloud variant uses prebuilt `udbax/ceilingio-base:*` images).
- `Dockerfile.dev` (Debug), `Dockerfile.prod` (Release), `Dockerfile.cloud` (Release, prebuilt base, runs as non-root `appuser`) — each builds only `ceilingIOServer`.
- Compose files wire the base + server images together; `.dev`/`.prod` use an **external** network named `mastering_webapp_network` (must exist before `up`), `.cloud` uses `mastering_network`.

Server listens on 8080 and is resource-limited (1–2 CPU, 1.5–2 GB) in the compose files.

## Things to watch for

- Don't assume `ceilingIO_FETCH_SERVER_DEPS` does anything in CMake.
- The root `Dockerfile` is out of date (wrong target name) — fix or avoid it.
- JUCE GUI modules are explicitly disabled (`JUCE_WEB_BROWSER=0`, `juce_gui_*` unavailable) — this is a headless DSP/service project; don't introduce GUI dependencies.