# Docker & Docker Compose Skill (Workspace)

Purpose
-------
Capture a compact, repeatable workflow for building, iterating, debugging, and optimizing containerized C++ projects using Docker and Docker Compose in this repository.

When To Use
-----------
- Setting up development or CI container images for the project.
- Iterating on C++ code without rebuilding heavy third-party deps (JUCE) on each change.
- Optimizing Dockerfiles and Compose files for build caching, layer reuse, and multi-stage builds.

Step-by-step Workflow
---------------------
1. Design base images
   - Move heavy, rarely-changing build prerequisites (toolchain, JUCE clone, package installs) into a dedicated `Dockerfile.base` with named stages like `AS build` and `AS runtime`.
   - Tag the produced images (for example `ceilingio/base:build` and `ceilingio/base:runtime`) so other Dockerfiles can reference them with `ARG` defaults.

2. Multi-stage project Dockerfiles
   - Keep `Dockerfile.dev` and `Dockerfile.prod` minimal: they should `FROM` the base images (via `ARG`), copy project files, run CMake configure and build, then copy the final binary into the runtime stage.
   - Use CMake build directories consistent with the base image's JUCE layout (e.g., `-DJUCE_DIR=/opt/JUCE`).

3. Compose orchestration
   - Create `docker-compose.dev.yml` and `docker-compose.prod.yml` that first build the `Dockerfile.base` targets (with `target: build` and `target: runtime`) and tag them, then build the server image which depends_on those base services.
   - For development, add a `ceilingio-dev` service (see below) that mounts source and a persistent `build` volume so you can incrementally rebuild without re-cloning or reconfiguring JUCE.

4. Fast iteration patterns (avoid rebuilding JUCE)
   - Option A (recommended): Run builds inside a running base-image container and mount host sources + a persistent `build` volume and `ccache` folder. Then run `cmake --build` inside the container repeatedly.
   - Option B: Prebuild and `install` JUCE in `Dockerfile.base` and link against the installed files; rebuild base image only when JUCE changes.
   - Option C: Develop on host (native toolchain) and use containerized CI for reproducible builds.

5. Layer and cache optimization
   - Group apt installs into a single RUN layer and cleanup `apt-get` lists in the same step to prevent cache invalidation from unrelated changes.
   - Clone large repos (JUCE) in an early layer so later code changes don't invalidate that cache.
   - Use `.dockerignore` to avoid copying heavy files into the build context.

6. Build-time args and tags
   - Use `ARG BASE_BUILD_IMAGE=ceilingio/base:build` and `ARG BASE_RUNTIME_IMAGE=ceilingio/base:runtime` in `Dockerfile.dev`/`prod` so you can override images (CI or local) without changing files.
   - Tag base images consistently and push them to your registry if CI reuses them across runners.

7. Debugging common failure modes
   - "No rule to make target": check that the CMake target name (`ceilingIOServer`) matches the `cmake --build --target` argument and that the working directory is correct.
   - Missing JUCE or invalid `JUCE_DIR`: ensure `JUCE_DIR` is present in the base image and passed into the configure step via `-DJUCE_DIR=/opt/JUCE`.
   - Long build times: ensure heavy deps are in `Dockerfile.base` and that you use mounted `build` volumes and `ccache` during dev.

8. Compose dev service example (incremental builds)
   - Add the following `ceilingio-dev` service to `docker-compose.dev.yml` to iterate quickly without rebuilding images:

```yaml
  ceilingio-dev:
    image: ceilingio/base:build
    working_dir: /src/ceilingIO
    volumes:
      - ./:/src/ceilingIO:cached
      - ceilingio_build_cache:/src/ceilingIO/build
      - ceilingio_ccache:/root/.ccache
    entrypoint: ["/bin/sh", "-c"]
    command: >
      cmake -B build -DCMAKE_BUILD_TYPE=Debug -DJUCE_DIR=/opt/JUCE -DceilingIO_FETCH_SERVER_DEPS=ON &&
      cmake --build build --target ceilingIOServer -j"$(nproc)"

volumes:
  ceilingio_build_cache:
  ceilingio_ccache:
```

Usage:
```
docker compose -f docker-compose.dev.yml run --rm ceilingio-dev
```
or create a long-running container and exec incremental builds for ultra-fast iteration:
```
docker compose -f docker-compose.dev.yml run --rm -d --name dev-runner ceilingio-dev sleep infinity
docker exec -it dev-runner /bin/sh -c 'cmake --build build --target ceilingIOServer -j4'
```

9. CI recommendations
   - Build and push `ceilingio/base:build` and `ceilingio/base:runtime` in a dedicated CI job when base requirements change.
   - Use `docker buildx` for multi-platform images if needed.
   - In CI, run a full `docker-compose -f docker-compose.prod.yml build` and `docker-compose -f docker-compose.prod.yml push` for reproducible production images.

Checklist / Quality Gates
-------------------------
- Base image builds succeed and are cached separately from project builds.
- Dev workflow builds only the application objects after code changes (no JUCE rebuild).
- Compose files reference `Dockerfile.dev`/`Dockerfile.prod` and not the legacy `Dockerfile` (leave legacy for archival only).
- `.dockerignore` is present and excludes build artifacts and large files.

Example prompts to use this skill
--------------------------------
- "Add the `ceilingio-dev` service to docker-compose.dev.yml that mounts build and ccache volumes."  
- "Prebuild JUCE in Dockerfile.base and install to /opt/juce-install; update Dockerfile.dev to use it."  
- "Create a CI job that builds and pushes ceilingio/base images."  

Questions for clarification
---------------------------
- Do you want the dev service to run `cmake` then exit, or keep a long-running container for manual execs?  
- Do you prefer prebuilding JUCE in the base image (build-time) or mounting a persistent build dir for fast dev iteration?  

How to iterate on this skill
---------------------------
1. Apply the `ceilingio-dev` example to `docker-compose.dev.yml` and try an incremental run.  
2. If iterative dev is prioritized, add `ccache` and persistent volumes and document commands in README.  
3. If reproducibility is prioritized, prebuild and install JUCE in `Dockerfile.base` and add CI jobs to push the base image.

License
-------
This skill file is a developer aid and inherits the repository license where applicable.
