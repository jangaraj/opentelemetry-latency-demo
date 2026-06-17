//! otel-http-client is a minimal OpenTelemetry-instrumented HTTP client.
//!
//! It performs a GET against TEST_URL on a fixed interval (TEST_PERIOD) and
//! emits traces, metrics (request counter + latency histogram) and logs over
//! OTLP. Exporter configuration (endpoint, headers, timeout) is read from the
//! standard OTEL_* environment variables by the OTLP HTTP exporter.

use std::time::{Duration, Instant};

use opentelemetry::metrics::{Counter, Histogram};
use opentelemetry::propagation::{Injector, TextMapCompositePropagator};
use opentelemetry::trace::{SpanKind, Status, TraceContextExt, Tracer};
use opentelemetry::{global, Context, KeyValue};
use opentelemetry_appender_tracing::layer::OpenTelemetryTracingBridge;
use opentelemetry_otlp::{LogExporter, MetricExporter, SpanExporter};
use opentelemetry_sdk::logs::SdkLoggerProvider;
use opentelemetry_sdk::metrics::{Aggregation, Instrument, SdkMeterProvider, Stream};
use opentelemetry_sdk::propagation::{BaggagePropagator, TraceContextPropagator};
use opentelemetry_sdk::trace::SdkTracerProvider;
use opentelemetry_sdk::Resource;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::signal;
use tracing::{error, info};
use tracing_subscriber::prelude::*;
use tracing_subscriber::EnvFilter;
use url::Url;

const INSTRUMENTATION_NAME: &str = "otel-tracing";
const DEFAULT_CLIENT_NAME: &str = "rust-client";
const DEFAULT_TEST_URL: &str = "https://example.com/";
const DEFAULT_OTLP_ENDPOINT: &str = "http://localhost:4318";
const DEFAULT_OTLP_PROTOCOL: &str = "http/protobuf";
const DEFAULT_PERIOD: Duration = Duration::from_secs(1);

/// Explicit histogram bucket boundaries (in seconds) for the
/// `test.client.*.duration` histograms. Tuned for fast HTTP requests
/// (roughly 1ms..5s) instead of the SDK default 0..10000 buckets.
const DURATION_BOUNDARIES: &[f64] = &[
    0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.075, 0.1, 0.25, 0.5, 0.75, 1.0, 2.5, 5.0,
];

/// Parses a Go-style duration string (e.g. "1s", "500ms", "2m"). Supported
/// units: ms, s, m, h. Returns None for invalid or non-positive values.
fn parse_period(text: &str) -> Option<Duration> {
    let split = text
        .find(|c: char| c.is_alphabetic())
        .unwrap_or(text.len());
    let (number, unit) = text.split_at(split);
    let value: f64 = number.trim().parse().ok()?;
    let millis = match unit {
        "ms" => value,
        "s" | "" => value * 1_000.0,
        "m" => value * 60_000.0,
        "h" => value * 3_600_000.0,
        _ => return None,
    };
    if millis <= 0.0 {
        return None;
    }
    Some(Duration::from_millis(millis as u64))
}

/// Formats a duration for logs (e.g. "1s", "500ms"), matching Go's duration string style.
fn format_period(period: Duration) -> String {
    let millis = period.as_millis();
    if millis % 1000 == 0 {
        format!("{}s", millis / 1000)
    } else {
        format!("{millis}ms")
    }
}

/// Returns the host name, mirroring Go's resource.WithHost().
fn host_name() -> String {
    std::env::var("HOSTNAME")
        .or_else(|_| std::env::var("COMPUTERNAME"))
        .unwrap_or_else(|_| "unknown".into())
}

/// Returns a runtime description, mirroring Go's resource.WithProcessRuntimeDescription().
fn runtime_description() -> String {
    format!(
        "rust {} {}",
        std::env::consts::OS,
        std::env::consts::ARCH
    )
}

/// Per-phase durations of a single request, in seconds.
#[derive(Default)]
struct RequestTimings {
    dns: f64,
    connect: f64,
    tls: f64,
    ttfb: f64,
}

/// Carrier used to inject the W3C trace context into raw HTTP request headers.
struct HeaderCarrier {
    headers: Vec<(String, String)>,
}

impl Injector for HeaderCarrier {
    fn set(&mut self, key: &str, value: String) {
        self.headers.push((key.to_string(), value));
    }
}

/// Builds the resource shared by all signals. Mirrors Go's resource.New setup:
/// client default, OTEL_RESOURCE_ATTRIBUTES / OTEL_SERVICE_NAME, SDK, host, and
/// process runtime attributes.
fn build_resource() -> Resource {
    if std::env::var_os("OTEL_SERVICE_NAME").is_none() {
        std::env::set_var("OTEL_SERVICE_NAME", DEFAULT_CLIENT_NAME);
    }
    // Resource::builder() includes SdkProvidedResourceDetector,
    // TelemetryResourceDetector, and EnvResourceDetector by default.
    Resource::builder()
        .with_attribute(KeyValue::new("client", DEFAULT_CLIENT_NAME))
        .with_attribute(KeyValue::new("host.name", host_name()))
        .with_attribute(KeyValue::new(
            "process.runtime.description",
            runtime_description(),
        ))
        .build()
}

fn init_traces(resource: Resource) -> SdkTracerProvider {
    let exporter = SpanExporter::builder()
        .with_http()
        .build()
        .expect("failed to build span exporter");
    SdkTracerProvider::builder()
        .with_resource(resource)
        .with_batch_exporter(exporter)
        .build()
}

fn init_metrics(resource: Resource) -> SdkMeterProvider {
    let exporter = MetricExporter::builder()
        .with_http()
        .build()
        .expect("failed to build metric exporter");
    // Apply custom bucket boundaries to every test.client.*.duration histogram.
    let duration_view = |instrument: &Instrument| {
        if instrument.name().ends_with(".duration") {
            Stream::builder()
                .with_aggregation(Aggregation::ExplicitBucketHistogram {
                    boundaries: DURATION_BOUNDARIES.to_vec(),
                    record_min_max: true,
                })
                .build()
                .ok()
        } else {
            None
        }
    };
    SdkMeterProvider::builder()
        .with_resource(resource)
        .with_periodic_exporter(exporter)
        .with_view(duration_view)
        .build()
}

fn init_logs(resource: Resource) -> SdkLoggerProvider {
    let exporter = LogExporter::builder()
        .with_http()
        .build()
        .expect("failed to build log exporter");
    SdkLoggerProvider::builder()
        .with_resource(resource)
        .with_batch_exporter(exporter)
        .build()
}

/// Per-phase latency instruments (DNS / connect / TLS / TTFB).
struct PhaseHistograms {
    dns: Histogram<f64>,
    connect: Histogram<f64>,
    tls: Histogram<f64>,
    ttfb: Histogram<f64>,
}

/// TestClient executes and instruments a single GET request per call.
struct TestClient {
    url: String,
    counter: Counter<u64>,
    histogram: Histogram<f64>,
    phases: PhaseHistograms,
}

impl TestClient {
    async fn do_request(&self) {
        let tracer = global::tracer(INSTRUMENTATION_NAME);
        let span = tracer
            .span_builder("test-request")
            .with_kind(SpanKind::Client)
            .start(&tracer);
        let cx = Context::current_with_span(span);
        let _guard = cx.clone().attach();

        cx.span()
            .set_attribute(KeyValue::new("http.request.method", "GET"));
        cx.span()
            .set_attribute(KeyValue::new("url.full", self.url.clone()));

        let start = Instant::now();
        let outcome = match tokio::time::timeout(Duration::from_secs(2), self.fetch(&cx)).await {
            Ok(inner) => inner,
            Err(_) => Err("timeout".to_string()),
        };
        let elapsed = start.elapsed().as_secs_f64();

        let mut attrs = vec![
            KeyValue::new("http.request.method", "GET"),
            KeyValue::new("url.full", self.url.clone()),
        ];

        match outcome {
            Ok((status, timings)) => {
                let status = status as i64;
                attrs.push(KeyValue::new("http.response.status_code", status));
                cx.span()
                    .set_attribute(KeyValue::new("http.response.status_code", status));
                if status >= 400 {
                    attrs.push(KeyValue::new("error.type", "http_error"));
                    cx.span()
                        .set_status(Status::error(format!("HTTP {status}")));
                }

                self.counter.add(1, &attrs);
                self.histogram.record(elapsed, &attrs);
                self.record_phases(&cx, &attrs, &timings);

                if status >= 400 {
                    error!(
                        url = %self.url,
                        status_code = status,
                        duration_s = elapsed,
                        dns_s = timings.dns,
                        connect_s = timings.connect,
                        tls_s = timings.tls,
                        ttfb_s = timings.ttfb,
                        "request failed"
                    );
                } else {
                    info!(
                        url = %self.url,
                        status_code = status,
                        duration_s = elapsed,
                        dns_s = timings.dns,
                        connect_s = timings.connect,
                        tls_s = timings.tls,
                        ttfb_s = timings.ttfb,
                        "request completed"
                    );
                }
            }
            Err(err) => {
                attrs.push(KeyValue::new("error.type", "request_error"));
                cx.span().set_status(Status::error(err.clone()));

                self.counter.add(1, &attrs);
                self.histogram.record(elapsed, &attrs);
                error!(
                    url = %self.url,
                    duration_s = elapsed,
                    error = %err,
                    "request failed"
                );
            }
        }

        cx.span().end();
    }

    /// Records the per-phase timings on the active span and as metrics.
    fn record_phases(&self, cx: &Context, attrs: &[KeyValue], t: &RequestTimings) {
        cx.span()
            .set_attribute(KeyValue::new("dns.duration_s", t.dns));
        cx.span()
            .set_attribute(KeyValue::new("connect.duration_s", t.connect));
        cx.span()
            .set_attribute(KeyValue::new("tls.duration_s", t.tls));
        cx.span()
            .set_attribute(KeyValue::new("ttfb.duration_s", t.ttfb));
        self.phases.dns.record(t.dns, attrs);
        self.phases.connect.record(t.connect, attrs);
        self.phases.tls.record(t.tls, attrs);
        self.phases.ttfb.record(t.ttfb, attrs);
    }

    /// Performs the request manually over TCP (and TLS for https) so that the
    /// DNS, connect, TLS handshake and time-to-first-byte phases can each be
    /// measured. Returns the HTTP status code and the per-phase timings.
    async fn fetch(&self, cx: &Context) -> Result<(u16, RequestTimings), String> {
        let url = Url::parse(&self.url).map_err(|e| format!("parse url: {e}"))?;
        let host = url
            .host_str()
            .ok_or_else(|| "url has no host".to_string())?
            .to_string();
        let https = url.scheme() == "https";
        let port = url
            .port_or_known_default()
            .ok_or_else(|| "url has no port".to_string())?;
        let mut target = url.path().to_string();
        if target.is_empty() {
            target = "/".to_string();
        }
        if let Some(query) = url.query() {
            target.push('?');
            target.push_str(query);
        }

        let mut timings = RequestTimings::default();

        // DNS resolution.
        let t = Instant::now();
        let mut addrs = tokio::net::lookup_host((host.as_str(), port))
            .await
            .map_err(|e| format!("dns: {e}"))?;
        let addr = addrs
            .next()
            .ok_or_else(|| "dns: no addresses".to_string())?;
        timings.dns = t.elapsed().as_secs_f64();

        // TCP connection.
        let t = Instant::now();
        let stream = TcpStream::connect(addr)
            .await
            .map_err(|e| format!("connect: {e}"))?;
        timings.connect = t.elapsed().as_secs_f64();

        // Build the raw request with the injected trace context.
        let mut carrier = HeaderCarrier {
            headers: Vec::new(),
        };
        global::get_text_map_propagator(|propagator| {
            propagator.inject_context(cx, &mut carrier);
        });
        let mut request = format!(
            "GET {target} HTTP/1.1\r\nHost: {host}\r\nUser-Agent: otel-http-client\r\nAccept: */*\r\nConnection: close\r\n"
        );
        for (key, value) in &carrier.headers {
            request.push_str(&format!("{key}: {value}\r\n"));
        }
        request.push_str("\r\n");

        if https {
            let t = Instant::now();
            let connector = tokio_native_tls::TlsConnector::from(
                tokio_native_tls::native_tls::TlsConnector::new()
                    .map_err(|e| format!("tls setup: {e}"))?,
            );
            let mut tls = connector
                .connect(&host, stream)
                .await
                .map_err(|e| format!("tls: {e}"))?;
            timings.tls = t.elapsed().as_secs_f64();
            let status = exchange(&mut tls, request.as_bytes(), &mut timings).await?;
            Ok((status, timings))
        } else {
            let mut stream = stream;
            let status = exchange(&mut stream, request.as_bytes(), &mut timings).await?;
            Ok((status, timings))
        }
    }
}

/// Writes the request, measures the time to the first response byte (TTFB),
/// drains the rest of the response and parses the HTTP status code.
async fn exchange<S>(
    stream: &mut S,
    request: &[u8],
    timings: &mut RequestTimings,
) -> Result<u16, String>
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    let sent = Instant::now();
    stream
        .write_all(request)
        .await
        .map_err(|e| format!("write: {e}"))?;
    stream.flush().await.map_err(|e| format!("flush: {e}"))?;

    let mut first = [0u8; 1];
    let n = stream
        .read(&mut first)
        .await
        .map_err(|e| format!("read: {e}"))?;
    timings.ttfb = sent.elapsed().as_secs_f64();
    if n == 0 {
        return Err("empty response".to_string());
    }

    let mut buf = vec![first[0]];
    let mut chunk = [0u8; 8192];
    loop {
        let n = stream
            .read(&mut chunk)
            .await
            .map_err(|e| format!("read: {e}"))?;
        if n == 0 {
            break;
        }
        buf.extend_from_slice(&chunk[..n]);
    }

    parse_status(&buf)
}

/// Parses the status code from a raw HTTP response (e.g. "HTTP/1.1 200 OK").
fn parse_status(buf: &[u8]) -> Result<u16, String> {
    let text = String::from_utf8_lossy(buf);
    let line = text
        .lines()
        .next()
        .ok_or_else(|| "no status line".to_string())?;
    let code = line
        .split_whitespace()
        .nth(1)
        .ok_or_else(|| "no status code".to_string())?;
    code.parse::<u16>().map_err(|e| format!("bad status: {e}"))
}

/// Resolves once SIGINT or SIGTERM is received.
async fn shutdown_signal() {
    let ctrl_c = async {
        let _ = signal::ctrl_c().await;
    };

    #[cfg(unix)]
    let terminate = async {
        match signal::unix::signal(signal::unix::SignalKind::terminate()) {
            Ok(mut stream) => {
                stream.recv().await;
            }
            Err(_) => std::future::pending::<()>().await,
        }
    };

    #[cfg(not(unix))]
    let terminate = std::future::pending::<()>();

    tokio::select! {
        _ = ctrl_c => {},
        _ = terminate => {},
    }
}

#[tokio::main]
async fn main() {
    let test_url = std::env::var("TEST_URL").unwrap_or_else(|_| DEFAULT_TEST_URL.to_string());

    let period = match std::env::var("TEST_PERIOD") {
        Ok(value) if !value.is_empty() => match parse_period(&value) {
            Some(period) => period,
            None => {
                eprintln!("invalid TEST_PERIOD: {value}");
                std::process::exit(1);
            }
        },
        _ => DEFAULT_PERIOD,
    };

    if std::env::var_os("OTEL_EXPORTER_OTLP_ENDPOINT").is_none() {
        std::env::set_var("OTEL_EXPORTER_OTLP_ENDPOINT", DEFAULT_OTLP_ENDPOINT);
    }
    if std::env::var_os("OTEL_EXPORTER_OTLP_PROTOCOL").is_none() {
        std::env::set_var("OTEL_EXPORTER_OTLP_PROTOCOL", DEFAULT_OTLP_PROTOCOL);
    }

    let resource = build_resource();
    let tracer_provider = init_traces(resource.clone());
    let meter_provider = init_metrics(resource.clone());
    let logger_provider = init_logs(resource.clone());

    global::set_tracer_provider(tracer_provider.clone());
    global::set_meter_provider(meter_provider.clone());
    global::set_text_map_propagator(TextMapCompositePropagator::new(vec![
        Box::new(TraceContextPropagator::new()),
        Box::new(BaggagePropagator::new()),
    ]));

    // Route `tracing` events to both stderr and the OTLP log pipeline. The
    // filter keeps the bridge from re-ingesting the SDK's own internal logs.
    let otel_filter = EnvFilter::new("info")
        .add_directive("hyper=off".parse().unwrap())
        .add_directive("tonic=off".parse().unwrap())
        .add_directive("reqwest=off".parse().unwrap())
        .add_directive("opentelemetry=off".parse().unwrap());
    let otel_layer = OpenTelemetryTracingBridge::new(&logger_provider).with_filter(otel_filter);
    let fmt_layer = tracing_subscriber::fmt::layer()
        .with_writer(std::io::stderr)
        .with_filter(EnvFilter::new("info"));
    tracing_subscriber::registry()
        .with(otel_layer)
        .with(fmt_layer)
        .init();

    let meter = global::meter(INSTRUMENTATION_NAME);
    // Names are prefixed with test.client to mirror the Go example.
    let counter = meter
        .u64_counter("test.client.requests")
        .with_description("Number of HTTP requests executed by the test client")
        .with_unit("{request}")
        .build();
    let histogram = meter
        .f64_histogram("test.client.request.duration")
        .with_description("Total duration of HTTP requests executed by the test client")
        .with_unit("s")
        .build();
    let phases = PhaseHistograms {
        dns: meter
            .f64_histogram("test.client.dns.duration")
            .with_description("DNS resolution time")
            .with_unit("s")
            .build(),
        connect: meter
            .f64_histogram("test.client.connect.duration")
            .with_description("TCP connection establishment time")
            .with_unit("s")
            .build(),
        tls: meter
            .f64_histogram("test.client.tls.duration")
            .with_description("TLS handshake time")
            .with_unit("s")
            .build(),
        ttfb: meter
            .f64_histogram("test.client.ttfb.duration")
            .with_description("Time to first response byte (server wait time)")
            .with_unit("s")
            .build(),
    };

    let client = TestClient {
        url: test_url.clone(),
        counter,
        histogram,
        phases,
    };

    info!(
        url = %test_url,
        period = %format_period(period),
        "starting test client"
    );

    // Fire one request immediately, then on every tick.
    client.do_request().await;

    let mut ticker = tokio::time::interval_at(tokio::time::Instant::now() + period, period);
    loop {
        tokio::select! {
            _ = shutdown_signal() => {
                info!("shutting down");
                break;
            }
            _ = ticker.tick() => {
                client.do_request().await;
            }
        }
    }

    // Flush and stop all providers before exiting.
    let _ = tracer_provider.shutdown();
    let _ = meter_provider.shutdown();
    let _ = logger_provider.shutdown();
}
