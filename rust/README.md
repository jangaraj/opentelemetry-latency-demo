# OTel Rust HTTP Client

A minimal OpenTelemetry-instrumented HTTP client, the Rust counterpart of
[../golang](../golang). It issues a `GET` against a target URL on a fixed
interval and emits:

- **Traces** - one span per request (`test-request`, client kind), with the W3C
  `traceparent` header injected into the outgoing request. Each span carries the
  per-phase network timings as attributes: `dns.duration_s`,
  `connect.duration_s`, `tls.duration_s` and `ttfb.duration_s`.
- **Metrics**
  - `test.client.requests` - counter of executed requests.
  - `test.client.request.duration` - histogram of total request latency (seconds).
  - `test.client.dns.duration` - histogram of DNS resolution time (seconds).
  - `test.client.connect.duration` - histogram of TCP connection time (seconds).
  - `test.client.tls.duration` - histogram of TLS handshake time (seconds; `0` for plain HTTP).
  - `test.client.ttfb.duration` - histogram of time to first response byte / server wait time (seconds).
- **Logs** - structured `tracing` events, written to stderr and exported via OTLP.
  Each line includes `dns_s`, `connect_s`, `tls_s` and `ttfb_s` fields.

All `*.duration` histograms use custom explicit bucket boundaries (in seconds)
tuned for fast HTTP requests - `0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.075,
0.1, 0.25, 0.5, 0.75, 1, 2.5, 5` - instead of the SDK default `0..10000` buckets.

The request is performed manually over TCP (and TLS for `https`) so that each
phase - DNS resolution, TCP connect, TLS handshake and time to first byte - can
be measured individually. A fresh connection (`Connection: close`) is used per
request.

Telemetry is exported using the OTLP **HTTP** exporter (`http/protobuf` by default), whose
endpoint/headers/timeout are read from the standard `OTEL_*` environment variables.

## Configuration

### Application

| Variable      | Required | Default                | Description                                          |
| ------------- | -------- | ---------------------- | ---------------------------------------------------- |
| `TEST_URL`    | no       | `https://example.com/` | Target URL to GET.                                   |
| `TEST_PERIOD` | no       | `1s`                   | Poll interval as a duration (e.g. `500ms`, `2s`, `1m`). |

All signals include the resource attribute `client=rust-client` by default
(override via `OTEL_RESOURCE_ATTRIBUTES`).

### OpenTelemetry (standard SDK env vars)

| Variable                              | Example                            | Description                              |
| ------------------------------------- | ---------------------------------- | ---------------------------------------- |
| `OTEL_EXPORTER_OTLP_ENDPOINT`         | `http://localhost:4318`            | Base OTLP HTTP endpoint (default if unset). |
| `OTEL_EXPORTER_OTLP_PROTOCOL`         | `http/protobuf`                          | OTLP encoding (default if unset: `http/protobuf`). |
| `OTEL_SERVICE_NAME`                   | `otel-http-client`                 | Service name (defaults to `rust-client`).|
| `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT`  | `http://localhost:4318/v1/traces`  | Per-signal endpoint override.            |
| `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT` | `http://localhost:4318/v1/metrics` | Per-signal endpoint override.            |
| `OTEL_EXPORTER_OTLP_LOGS_ENDPOINT`    | `http://localhost:4318/v1/logs`    | Per-signal endpoint override.            |
| `OTEL_EXPORTER_OTLP_HEADERS`          | `authorization=Basic ...`          | Headers (e.g. auth for Grafana Cloud).   |

> This example uses the OTLP HTTP exporter (default port `4318`). To use gRPC,
> enable the `grpc-tonic` feature on `opentelemetry-otlp` and swap `.with_http()`
> for `.with_tonic()` in `src/main.rs`.

## Build and run with Docker (recommended)

```bash
# from the repository root
docker build -t otel-http-client-rust otel-tracing/rust

docker run --rm \
  -e TEST_PERIOD=1s \
  -e OTEL_SERVICE_NAME=otel-http-client \
  otel-http-client-rust
```

Stop with `Ctrl+C`; the client flushes pending telemetry before exiting.

## Build locally

Requires a Rust toolchain (`rustup`/`cargo`), plus `pkg-config` and `libssl-dev`
(for `reqwest`'s native TLS).

```bash
cd otel-tracing/rust
cargo build --release

export TEST_PERIOD=1s
./target/release/otel-http-client
```

## Quick local collector (Docker)

Run an OpenTelemetry Collector that logs everything it receives:

```bash
cat > /tmp/otel-collector.yaml <<'EOF'
receivers:
  otlp:
    protocols:
      http:
        endpoint: 0.0.0.0:4318
exporters:
  debug:
    verbosity: detailed
service:
  pipelines:
    traces:  { receivers: [otlp], exporters: [debug] }
    metrics: { receivers: [otlp], exporters: [debug] }
    logs:    { receivers: [otlp], exporters: [debug] }
EOF

docker run --rm -p 4318:4318 \
  -v /tmp/otel-collector.yaml:/etc/otelcol-contrib/config.yaml \
  otel/opentelemetry-collector-contrib:latest
```
