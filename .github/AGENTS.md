# AGENTS.md — Agent instructions for Ceiling.io

Purpose: give AI coding agents concise, actionable guidance for working in this repository.

Quick links
- Project README: [README.md](README.md)
- Build configuration: [CMakeLists.txt](CMakeLists.txt)

Quick commands (recommended)
- Configure + build (out-of-source):

  mkdir -p build
  cd build
  cmake .. -DCMAKE_BUILD_TYPE=Release
  cmake --build . -- -j4

- Build a single target (from repo root):

  cmake --build build --target ceilingIOServer -- -j4

- To enable server dependency fetching (Crow/Asio):

  cmake -S . -B build -DceilingIO_FETCH_SERVER_DEPS=ON

Notes & conventions
- Language: C++20 (set in [CMakeLists.txt](CMakeLists.txt)).
- Build system: CMake + JUCE. Agents must not assume JUCE is vendored — use `JUCE_DIR` or the system package as needed.
- Primary targets: `ceilingIO` (GUI app), `ceilingIOCli` (CLI), `ceilingIOServer` (server). Update CMake when adding sources.
- Shared sources live in `src/` and `include/` contains headers used by targets.
- Prefer small, focused changes. When adding source files update `CMakeLists.txt` to include them.

What an agent should do (best practices)
- Link to existing docs rather than copy them; add short notes when repo-specific context matters.
- When suggesting build or run steps, include exact `cmake` commands and any required cache options (e.g. `JUCE_DIR` or `ceilingIO_FETCH_SERVER_DEPS`).
- If modifying build settings that affect JUCE or platform flags, ask before wide-reaching changes.
- Run minimal local builds to verify changes when possible (build single target).

Potential follow-ups (suggested customizations)
- Add `.github/copilot-instructions.md` with a short pointer to this file for GitHub Copilot users.
- Add a `skills/` entry that automates common tasks: configure+build, run server, run CLI smoke test.

If you want, I can create the `.github/copilot-instructions.md` and a simple `skills/` helper next.
