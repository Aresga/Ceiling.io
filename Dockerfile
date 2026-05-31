FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    g++ \
    git \
    libasound2-dev \
    libcurl4-openssl-dev \
    libfontconfig1-dev \
    libfreetype6-dev \
    libgl1-mesa-dev \
    libgtk-3-dev \
    libx11-dev \
    libxcomposite-dev \
    libxcursor-dev \
    libxdamage-dev \
    libxext-dev \
    libxfixes-dev \
    libxinerama-dev \
    libxrandr-dev \
    libssl-dev \
    libwebkit2gtk-4.1-dev \
    make \
    pkg-config \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /opt
RUN git clone --depth 1 https://github.com/juce-framework/JUCE.git

WORKDIR /src/ceilingIO
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    -DJUCE_DIR=/opt/JUCE \
    -DceilingIO_FETCH_SERVER_DEPS=ON

RUN cmake --build build --target ceilingIOServer -j"$(nproc)"

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libasound2t64 \
    libcurl4 \
    libfontconfig1 \
    libfreetype6 \
    libssl3 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/ceilingIO/build/ceilingIOServer_artefacts/Release/ceilingIOServer /usr/local/bin/ceilingIOServer

EXPOSE 8080

ENTRYPOINT ["/usr/local/bin/ceilingIOServer"]
CMD ["8080"]