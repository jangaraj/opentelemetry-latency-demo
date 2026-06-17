# Builds the Rust OTLP HTTP client.
#
# Build:  docker build -t otel-http-client-rust otel-tracing/rust
# Run:    docker run --rm \
#                 -e OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4318 \
#                 otel-http-client-rust

FROM rust:1-bookworm AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        pkg-config \
        libssl-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY Cargo.toml ./
COPY src ./src
RUN cargo build --release

FROM debian:bookworm-slim AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libssl3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /app/target/release/otel-http-client /usr/local/bin/otel-http-client

ENV TEST_URL=https://example.com/
ENV OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4318
ENV OTEL_EXPORTER_OTLP_PROTOCOL=http/protobuf
ENTRYPOINT ["/usr/local/bin/otel-http-client"]
