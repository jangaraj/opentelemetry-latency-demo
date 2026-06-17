# Builds opentelemetry-cpp (OTLP HTTP exporter) from source and then this app.
#
# Build:  docker build -t otel-http-client-cpp otel-tracing/cpp
# Run:    docker run --rm \
#                 -e OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4318 \
#                 otel-http-client-cpp

FROM ubuntu:24.04 AS build

ARG OTEL_CPP_VERSION=v1.16.1
ENV DEBIAN_FRONTEND=noninteractive
ENV GIT_SSL_NO_VERIFY=true

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        ca-certificates \
        libcurl4-openssl-dev \
        libssl-dev \
        libprotobuf-dev \
        protobuf-compiler \
        libabsl-dev \
        nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

# Build and install opentelemetry-cpp with the OTLP HTTP exporter.
RUN git clone --depth 1 --branch ${OTEL_CPP_VERSION} --recursive \
        https://github.com/open-telemetry/opentelemetry-cpp.git /tmp/otel-cpp \
    && cmake -S /tmp/otel-cpp -B /tmp/otel-cpp/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DWITH_EXAMPLES=OFF \
        -DWITH_OTLP_HTTP=ON \
        -DWITH_OTLP_GRPC=OFF \
        -DWITH_ABSEIL=ON \
        -DOPENTELEMETRY_INSTALL=ON \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    && cmake --build /tmp/otel-cpp/build --target install -j "$(nproc)" \
    && rm -rf /tmp/otel-cpp

WORKDIR /app
COPY CMakeLists.txt main.cpp ./
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j "$(nproc)"

FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
# Install the runtime shared libraries via apt instead of copying arch-specific
# multiarch paths from the build stage, so the image builds on any architecture
# (amd64/arm64). opentelemetry-cpp is linked statically into the binary, so only
# curl, protobuf and abseil are required at runtime.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libcurl4t64 \
        libprotobuf32t64 \
        libabsl20220623t64 \
    && rm -rf /var/lib/apt/lists/*

# opentelemetry-cpp installs into /usr/local/lib. This is a no-op for the
# default static build, but keeps the image correct if shared libraries are
# enabled later.
COPY --from=build /usr/local/lib/ /usr/local/lib/
RUN ldconfig
COPY --from=build /app/build/otel-http-client /usr/local/bin/otel-http-client

ENV TEST_URL=https://example.com/
ENV OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4318
ENV OTEL_EXPORTER_OTLP_PROTOCOL=http/protobuf
ENTRYPOINT ["/usr/local/bin/otel-http-client"]
