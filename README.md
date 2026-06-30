# Ceiling.io

High-performance audio mastering engine built in C++ with JUCE.

Ceiling.io analyzes, normalizes, and masters audio to hit loudness targets for streaming platforms — Spotify, Apple Music, YouTube, Tidal — without clipping or losing dynamics. It runs as a desktop app, a CLI tool, or a headless server you can drop into any pipeline.

## What it does

- **LUFS analysis** — Measures integrated loudness, RMS, peak, and true peak (EBU R128–style)
- **Smart normalization** — Applies gain based on a platform's target LUFS, with a lookahead limiter that clamps true peaks at the ceiling
- **Genre-aware EQ & compression** — Applies genre-tuned tonal shaping and dynamics processing, not just a volume knob
- **Webhook-ready server mode** — Submit jobs over REST, poll for status, get callbacks when done

## Quick start

### Prerequisites

- C++20 compiler
- CMake ≥ 3.15
- [JUCE](https://github.com/juce-framework/JUCE) (cloned or installed via `CMAKE_PREFIX_PATH`)
- LAME (libmp3lame-dev) for MP3 output on the server target

### Build

```bash
# GUI app (desktop)
cmake -B build -DJUCE_DIR=/path/to/JUCE
cmake --build build --target ceilingIO

# CLI tool
cmake --build build --target ceilingIOCli

# Server (fetches Crow & Asio automatically)
cmake -B build -DceilingIO_FETCH_SERVER_DEPS=ON
cmake --build build --target ceilingIOServer
```

### CLI usage

```bash
# Analyze a file
ceilingIOCli --input track.wav

# Normalize for a specific platform
ceilingIOCli --input track.wav --output mastered.wav --preset spotify

# See available presets
ceilingIOCli --list-presets
```

### Server

```bash
ceilingIOServer 8080
```

POST a job:

```json
POST /api/v1/master/jobs
{
  "trackId": "abc-123",
  "inputUrl": "https://storage.example.com/raw/track.wav",
  "outputUrl": "https://storage.example.com/mastered/",
  "outputKey": "track_mastered",
  "outputFormat": "WAV",
  "targetLoudness": -14.0,
  "platform": "spotify",
  "genre": "pop",
  "callbackUrl": "https://api.example.com/webhook",
  "idempotencyKey": "unique-key"
}
```

Check status:

```
GET /api/v1/master/jobs/{jobId}
GET /health
```

### Docker

```bash
docker compose up
```

Runs the server on port 8080 out of the box.

## License

[AGPL-3.0](LICENSE)
