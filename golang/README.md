# OTel Go HTTP Client

A minimal OpenTelemetry-instrumented HTTP client. It issues a `GET` against a
target URL on a fixed interval and emits:

- **Traces** - one client span per request (`test-request`), with W3C trace context
  injected into the outgoing request. Each span carries the per-phase network
  timings as attributes: `dns.duration_s`, `connect.duration_s`,
  `tls.duration_s` and `ttfb.duration_s`.
- **Metrics**
  - `test.client.requests` - counter of executed requests.
  - `test.client.request.duration` - histogram of total request latency (seconds).
  - `test.client.dns.duration` - histogram of DNS resolution time (seconds).
  - `test.client.connect.duration` - histogram of TCP connection time (seconds).
  - `test.client.tls.duration` - histogram of TLS handshake time (seconds; `0` for plain HTTP).
  - `test.client.ttfb.duration` - histogram of time to first response byte / server wait time (seconds).
- **Logs** - structured `slog` lines, mirrored to stderr and exported via OTLP.
  Each line includes `dns_s`, `connect_s`, `tls_s` and `ttfb_s` fields.

All `*.duration` histograms use custom explicit bucket boundaries (in seconds)
tuned for fast HTTP requests - `0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.075,
0.1, 0.25, 0.5, 0.75, 1, 2.5, 5` - instead of the SDK default `0..10000` buckets.

Keep-alives are disabled so every request performs a fresh DNS lookup, TCP
connection and TLS handshake, making the per-phase timings meaningful on each
iteration.

All exporter configuration is driven by the standard `OTEL_*` environment
variables (via [`autoexport`](https://pkg.go.dev/go.opentelemetry.io/contrib/exporters/autoexport)),
so you control protocol and endpoints without code changes.

## Configuration

### Application

| Variable      | Required | Default | Description                                            |
| ------------- | -------- | ------- | ------------------------------------------------------ |
| `TEST_URL`    | no       | `https://example.com/` | Target URL to GET.                              |
| `TEST_PERIOD` | no       | `1s`    | Poll interval as a Go duration (e.g. `500ms`, `2s`).   |

All signals include the resource attribute `client=golang-client` by default (override via `OTEL_RESOURCE_ATTRIBUTES`).

### OpenTelemetry (standard SDK env vars)

| Variable                              | Example                  | Description                                          |
| ------------------------------------- | ------------------------ | ---------------------------------------------------- |
| `OTEL_EXPORTER_OTLP_ENDPOINT`         | `http://localhost:4318`  | Base OTLP endpoint for all signals (default if unset). |
| `OTEL_EXPORTER_OTLP_PROTOCOL`         | `http/protobuf` / `http/json` / `grpc` | OTLP encoding and transport (default if unset: `http/protobuf`). |
| `OTEL_SERVICE_NAME`                   | `otel-http-client`       | Service name (defaults to `otel-http-client`).       |
| `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT`  | `http://localhost:4318/v1/traces`  | Per-signal endpoint override.            |
| `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT` | `http://localhost:4318/v1/metrics` | Per-signal endpoint override.            |
| `OTEL_EXPORTER_OTLP_LOGS_ENDPOINT`    | `http://localhost:4318/v1/logs`    | Per-signal endpoint override.            |
| `OTEL_EXPORTER_OTLP_HEADERS`          | `authorization=Basic ...`          | Headers (e.g. auth for Grafana Cloud).   |

> Note: with `grpc`, point the endpoint at the gRPC port (default `4317`), e.g.
> `OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4317`.

You can also switch to console exporters for quick local debugging:
`OTEL_TRACES_EXPORTER=console`, `OTEL_METRICS_EXPORTER=console`,
`OTEL_LOGS_EXPORTER=console`.

## Run

Requires Go 1.25+.

```bash
# TEST_URL defaults to https://example.com/
# OTEL_EXPORTER_OTLP_ENDPOINT defaults to http://localhost:4318
# OTEL_EXPORTER_OTLP_PROTOCOL defaults to http/protobuf
export TEST_PERIOD="1s"
export OTEL_SERVICE_NAME="otel-http-client"

go run .
```

Stop with `Ctrl+C`; the client flushes pending telemetry before exiting.

### Quick local collector (Docker)

Run an OpenTelemetry Collector that logs everything it receives:

```bash
cat > /tmp/otel-collector.yaml <<'EOF'
receivers:
  otlp:
    protocols:
      http:
        endpoint: 0.0.0.0:4318
      grpc:
        endpoint: 0.0.0.0:4317
exporters:
  debug:
    verbosity: detailed
service:
  pipelines:
    traces:  { receivers: [otlp], exporters: [debug] }
    metrics: { receivers: [otlp], exporters: [debug] }
    logs:    { receivers: [otlp], exporters: [debug] }
EOF

docker run --rm -p 4317:4317 -p 4318:4318 \
  -v /tmp/otel-collector.yaml:/etc/otelcol-contrib/config.yaml \
  otel/opentelemetry-collector-contrib:latest
```

Then run the client (in another shell) with
`OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4318` and watch the collector logs.
