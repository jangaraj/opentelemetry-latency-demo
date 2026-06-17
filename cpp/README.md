# OTel C++ HTTP Client

A minimal OpenTelemetry-instrumented HTTP client, the C++ counterpart of
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
- **Logs** - structured lines, written to stderr and exported via OTLP.
  Each line includes `dns_s`, `connect_s`, `tls_s` and `ttfb_s` fields.

All `*.duration` histograms use custom explicit bucket boundaries (in seconds)
tuned for fast HTTP requests - `0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.075,
0.1, 0.25, 0.5, 0.75, 1, 2.5, 5` - instead of the SDK default `0..10000` buckets.

The phase timings are read from libcurl (`CURLINFO_*_TIME_T`); a fresh easy
handle per request ensures each iteration measures a full DNS lookup, TCP
connection and TLS handshake.

Telemetry is exported using the OTLP **HTTP** exporter (`http/protobuf` by default), whose
endpoint/headers are read from the standard `OTEL_*` environment variables.

## Configuration

### Application

| Variable      | Required | Default                | Description                                          |
| ------------- | -------- | ---------------------- | ---------------------------------------------------- |
| `TEST_URL`    | no       | `https://example.com/` | Target URL to GET.                                   |
| `TEST_PERIOD` | no       | `1s`                   | Poll interval as a duration (e.g. `500ms`, `2s`, `1m`). |

All signals include the resource attribute `client=cpp-client` by default
(override via `OTEL_RESOURCE_ATTRIBUTES`).

### OpenTelemetry (standard SDK env vars)

| Variable                              | Example                            | Description                              |
| ------------------------------------- | ---------------------------------- | ---------------------------------------- |
| `OTEL_EXPORTER_OTLP_ENDPOINT`         | `http://localhost:4318`            | Base OTLP HTTP endpoint (default if unset). |
| `OTEL_EXPORTER_OTLP_PROTOCOL`         | `http/protobuf`                          | OTLP encoding (default if unset: `http/protobuf`). |
| `OTEL_SERVICE_NAME`                   | `otel-http-client`                 | Service name (defaults to `cpp-client`). |
| `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT`  | `http://localhost:4318/v1/traces`  | Per-signal endpoint override.            |
| `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT` | `http://localhost:4318/v1/metrics` | Per-signal endpoint override.            |
| `OTEL_EXPORTER_OTLP_LOGS_ENDPOINT`    | `http://localhost:4318/v1/logs`    | Per-signal endpoint override.            |
| `OTEL_EXPORTER_OTLP_HEADERS`          | `authorization=Basic ...`          | Headers (e.g. auth for Grafana Cloud).   |

> This example uses the OTLP HTTP exporter. The endpoint must point at an
> OTLP/HTTP receiver (default port `4318`). To use gRPC instead, rebuild
> opentelemetry-cpp with `-DWITH_OTLP_GRPC=ON` and swap the exporter factories
> in `main.cpp`.

## Build and run with Docker (recommended)

Building opentelemetry-cpp from source is the bulk of the work, so a `Dockerfile`
is provided.

```bash
# from the repository root
docker build -t otel-http-client-cpp otel-tracing/cpp

docker run --rm \
  -e TEST_PERIOD=1s \
  -e OTEL_SERVICE_NAME=otel-http-client \
  otel-http-client-cpp
```

Stop with `Ctrl+C`; the client flushes pending telemetry before exiting.

## Build locally

Requires a C++17 compiler, CMake >= 3.16, libcurl, and an installed
opentelemetry-cpp with the OTLP HTTP exporter.

### Dependencies via vcpkg

```bash
vcpkg install "opentelemetry-cpp[otlp-http]" curl
cmake -S otel-tracing/cpp -B otel-tracing/cpp/build \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build otel-tracing/cpp/build
```

### Run

```bash
export TEST_PERIOD=1s
export OTEL_SERVICE_NAME=otel-http-client
./otel-tracing/cpp/build/otel-http-client
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
