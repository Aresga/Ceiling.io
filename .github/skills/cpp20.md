# C++20 Best Practices (Workspace Skill)

Purpose
-------
Provide a concise, repeatable workflow and checklist for writing, building, and reviewing modern C++20 code in this repository. This skill is workspace-scoped: it documents conventions, tooling, and decision points developers should follow when contributing C++ code here.

When To Use
-----------
- Adding or changing C++ sources, headers, or CMake targets.
- Onboarding new maintainers or running code reviews/CI checks.
- Setting up local dev flows that avoid rebuilding heavy dependencies (JUCE) during iteration.

Step-by-step Workflow
---------------------
1. Local setup (one-time)
   - Install a recent compiler with good C++20 support (GCC >= 11, Clang >= 13, or recent MSVC). Prefer the distro-provided packages or a pinned toolchain in CI.
   - Install CMake >= 3.18 (higher is better) and `ninja` for faster builds.
   - Enable `ccache` and create a persistent cache location for dev containers or CI.

2. Project/CMake conventions
   - Use target-oriented CMake: `add_library`/`add_executable` + `target_include_directories`, `target_compile_features`, `target_compile_options`, and `target_link_libraries` with `PUBLIC`/`PRIVATE`/`INTERFACE`.
   - Set `CMAKE_CXX_STANDARD 20` and `CMAKE_CXX_STANDARD_REQUIRED ON` in the top-level `CMakeLists.txt`.
   - Prefer `Ninja` generator for local builds: `cmake -G Ninja -S . -B build`.
   - Keep heavy third-party deps outside incremental build trees (prebuild or install JUCE in a base image or a mounted build dir).

3. Compiler flags & sanitizers
   - Development flags: `-Wall -Wextra -Wpedantic -Werror` (consider `-Wno-unused-parameter` where appropriate).
   - Use `-g -O0` for debug builds; `-O2 -g` for local debug-with-optimizations when needed.
   - Enable sanitizers in CI/dev branches for testing: `-fsanitize=address,undefined,leak` and, for threads, `-fsanitize=thread` (separate runs).

4. C++20 language features (how & when)
   - Use `auto` for type deduction when it improves readability; prefer explicit types for public APIs.
   - Prefer `std::span` for non-owning contiguous ranges instead of raw pointers + size when possible.
   - Use `std::ranges` and range-based algorithms for clean iteration and transformations.
   - Use `concepts` to express template constraints for public generic APIs; keep internal helper templates simple.
   - Use `std::move`/`std::forward` correctly — follow the ownership semantics: move for transfer-of-ownership, copy otherwise.
   - Use `constexpr`, `consteval` and `constinit` for compile-time constants and evaluation when beneficial.
   - Consider `coroutines` only when they simplify async/control-flow; add lightweight wrappers for integration with existing threading models.
   - Avoid using experimental/unstable features without a strong reason (document them if used).

5. Code organization & style
   - Keep headers minimal: forward-declare where possible, reduce transitive includes.
   - Use the PIMPL/opaque pointer pattern for large classes when ABI stability or compile-time cost matters.
   - Prefer small, testable functions. Aim for single responsibility per function.
   - Document thread-safety and ownership in headers (who owns memory, which functions are thread-safe).

6. Build iteration patterns (don’t rebuild JUCE every change)
   - Option A (recommended): Use a prebuilt base image or preinstalled JUCE and mount a persistent `build` directory for incremental builds. This reuses JUCE build artifacts and only recompiles your code changes.
   - Option B: Prebuild and `install` JUCE into the base image and have the project link against the installed JUCE. Rebuilding JUCE requires rebuilding the base image.
   - Option C: Develop on host toolchain for fastest edits, keep CI building containerized images.

7. Testing, CI and quality gates
   - Add unit and integration tests where possible. Use `ctest` and keep tests fast and isolated.
   - Run static analysis (clang-tidy) in CI with a baseline file for long-lived warnings.
   - Fail CI on sanitizer regressions, compiler warnings, or formatting errors.

Decision Points & Branching Logic
---------------------------------
- When to use coroutines: prefer existing async libraries unless coroutines simplify the code path and remain well-tested.
- When to rebuild JUCE in CI: if JUCE version or configuration changes, rebuild base image; otherwise reuse prebuilt artifacts.
- When to enable sanitizers: run them on PR branches and nightly CI; avoid adding them to long-running deployment builds.

Quality Criteria / Completion Checks
-----------------------------------
- Builds cleanly with `cmake --build` using the recommended generator.
- No new compiler warnings (treat warnings as errors in PRs unless a clear justification is provided).
- Unit tests pass and coverage for modified code is reasonable.
- CI runs static analysis and sanitizer checks for changes touching critical code.

Example prompts to use this skill
---------------------------------
- "Run the C++20 skill checklist on my changes and list any missing items."  
- "Add a `ceilingio-dev` service to docker-compose.dev.yml that mounts the build directory and uses ccache."  
- "Suggest sanitizer-friendly compiler flags for CI for this repo." 

How to iterate on this skill
---------------------------
1. Start by using the prompts above to uncover missing or unclear items.
2. Update this `SKILL.md` with any repo-specific conventions (naming, formatting, license headers, etc.).
3. Add example `clang-tidy`/`clang-format`/`CMake` snippets to the repo under `.ci/` or `.dev/` for automated checks.

Files to add or update when adopting this skill
----------------------------------------------
- `docker-compose.dev.yml` — add a `ceilingio-dev` service that mounts the project and a persistent `build` volume.
- `.dockerignore` — exclude heavy artifacts not needed in build context.
- `.clang-format`, `.clang-tidy`, `ci/` pipeline snippets — for consistent formatting and analysis.

Questions for clarification
---------------------------
- Do you prefer host-local iteration (mounts) or strictly containerized builds for development?  
- Should sanitizers be enabled for all PRs or just nightly/selected branches?  
- Would you like me to add a ready-to-run `ceilingio-dev` service to `docker-compose.dev.yml` now?

License
-------
This skill file is a developer aid and inherits the repository license where applicable.