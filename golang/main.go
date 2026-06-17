// Command otel-http-client is a minimal OpenTelemetry-instrumented HTTP client.
//
// It performs a GET against TEST_URL on a fixed interval (TEST_PERIOD) and emits
// traces, metrics (request counter + latency histogram) and logs over OTLP. All
// exporter configuration is taken from the standard OTEL_* environment variables.
package main

import (
	"context"
	"crypto/tls"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptrace"
	"os"
	"os/signal"
	"syscall"
	"time"

	"go.opentelemetry.io/contrib/bridges/otelslog"
	"go.opentelemetry.io/contrib/exporters/autoexport"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/codes"
	otellog "go.opentelemetry.io/otel/log/global"
	"go.opentelemetry.io/otel/metric"
	"go.opentelemetry.io/otel/propagation"
	sdklog "go.opentelemetry.io/otel/sdk/log"
	sdkmetric "go.opentelemetry.io/otel/sdk/metric"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	semconv "go.opentelemetry.io/otel/semconv/v1.26.0"
	"go.opentelemetry.io/otel/trace"
)

const (
	// instrumentationName is the scope used for the tracer, meter and logger.
	instrumentationName = "otel-tracing"
	defaultClientName  = "golang-client"
	defaultTestURL     = "https://example.com/"
	defaultOTLPEndpoint  = "http://localhost:4318"
	defaultOTLPProtocol  = "http/protobuf"
	defaultPeriod      = time.Second
)

// durationBoundaries are the explicit histogram bucket boundaries (in seconds)
// used for all test.client.*.duration histograms. They are tuned for fast HTTP
// requests (roughly 1ms..5s) instead of the SDK default 0..10000 buckets.
var durationBoundaries = []float64{
	0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.075, 0.1, 0.25, 0.5, 0.75, 1, 2.5, 5,
}

func main() {
	if err := run(); err != nil {
		slog.Error("fatal", "error", err)
		os.Exit(1)
	}
}

func run() error {
	testURL := os.Getenv("TEST_URL")
	if testURL == "" {
		testURL = defaultTestURL
	}

	period := defaultPeriod
	if v := os.Getenv("TEST_PERIOD"); v != "" {
		d, err := time.ParseDuration(v)
		if err != nil {
			return fmt.Errorf("invalid TEST_PERIOD %q: %w", v, err)
		}
		if d <= 0 {
			return fmt.Errorf("TEST_PERIOD must be positive, got %q", v)
		}
		period = d
	}

	// Root context cancelled on SIGINT/SIGTERM for graceful shutdown.
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	shutdown, err := setupOTel(ctx)
	if err != nil {
		return fmt.Errorf("setup opentelemetry: %w", err)
	}
	// Flush telemetry on exit using a fresh context so pending data is exported
	// even after the root context has been cancelled.
	defer func() {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		if err := shutdown(shutdownCtx); err != nil {
			slog.Error("opentelemetry shutdown", "error", err)
		}
	}()

	// slog is wired to the OTel log pipeline in setupOTel, so these logs are
	// also exported via OTLP.
	logger := slog.Default()

	meter := otel.Meter(instrumentationName)
	requestCounter, err := meter.Int64Counter(
		"test.client.requests",
		metric.WithDescription("Number of HTTP requests executed by the test client"),
		metric.WithUnit("{request}"),
	)
	if err != nil {
		return fmt.Errorf("create request counter: %w", err)
	}
	latencyHistogram, err := meter.Float64Histogram(
		"test.client.request.duration",
		metric.WithDescription("Total duration of HTTP requests executed by the test client"),
		metric.WithUnit("s"),
	)
	if err != nil {
		return fmt.Errorf("create latency histogram: %w", err)
	}
	phaseHistograms, err := newPhaseHistograms(meter)
	if err != nil {
		return err
	}

	c := &testClient{
		// Keep-alives are disabled so every request performs a fresh DNS
		// lookup, TCP connection and TLS handshake, making the phase timings
		// meaningful on each iteration.
		httpClient: &http.Client{
			Timeout: 2 * time.Second,
			Transport: &http.Transport{
				Proxy:             http.ProxyFromEnvironment,
				DisableKeepAlives: true,
			},
		},
		url:       testURL,
		tracer:    otel.Tracer(instrumentationName),
		counter:   requestCounter,
		histogram: latencyHistogram,
		phases:    phaseHistograms,
		logger:    logger,
	}

	logger.Info("starting test client", "url", testURL, "period", period.String())

	// Fire one request immediately, then on every tick.
	c.doRequest(ctx)

	ticker := time.NewTicker(period)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			logger.Info("shutting down")
			return nil
		case <-ticker.C:
			c.doRequest(ctx)
		}
	}
}

// testClient bundles the dependencies needed to execute and instrument a request.
type testClient struct {
	httpClient *http.Client
	url        string
	tracer     trace.Tracer
	counter    metric.Int64Counter
	histogram  metric.Float64Histogram
	phases     phaseHistograms
	logger     *slog.Logger
}

// phaseHistograms holds the per-phase latency instruments for a request.
type phaseHistograms struct {
	dns     metric.Float64Histogram
	connect metric.Float64Histogram
	tls     metric.Float64Histogram
	ttfb    metric.Float64Histogram
}

// newPhaseHistograms creates the DNS / connect / TLS / TTFB latency histograms.
func newPhaseHistograms(meter metric.Meter) (phaseHistograms, error) {
	var (
		p   phaseHistograms
		err error
	)
	if p.dns, err = meter.Float64Histogram(
		"test.client.dns.duration",
		metric.WithDescription("DNS resolution time"),
		metric.WithUnit("s"),
	); err != nil {
		return p, fmt.Errorf("create dns histogram: %w", err)
	}
	if p.connect, err = meter.Float64Histogram(
		"test.client.connect.duration",
		metric.WithDescription("TCP connection establishment time"),
		metric.WithUnit("s"),
	); err != nil {
		return p, fmt.Errorf("create connect histogram: %w", err)
	}
	if p.tls, err = meter.Float64Histogram(
		"test.client.tls.duration",
		metric.WithDescription("TLS handshake time"),
		metric.WithUnit("s"),
	); err != nil {
		return p, fmt.Errorf("create tls histogram: %w", err)
	}
	if p.ttfb, err = meter.Float64Histogram(
		"test.client.ttfb.duration",
		metric.WithDescription("Time to first response byte (server wait time)"),
		metric.WithUnit("s"),
	); err != nil {
		return p, fmt.Errorf("create ttfb histogram: %w", err)
	}
	return p, nil
}

// requestTimings holds the per-phase durations of a single request, in seconds.
type requestTimings struct {
	dns     float64
	connect float64
	tls     float64
	ttfb    float64
}

// doRequest performs a single GET, recording a span, metrics and a log line.
func (c *testClient) doRequest(ctx context.Context) {
	ctx, span := c.tracer.Start(ctx, "test-request")
	defer span.End()

	start := time.Now()
	statusCode, timings, err := c.fetch(ctx)
	elapsed := time.Since(start).Seconds()

	attrs := []attribute.KeyValue{
		semconv.HTTPRequestMethodGet,
		semconv.URLFull(c.url),
	}
	if err != nil {
		attrs = append(attrs, attribute.String("error.type", "request_error"))
		span.RecordError(err)
		span.SetStatus(codes.Error, err.Error())
	} else {
		attrs = append(attrs, semconv.HTTPResponseStatusCode(statusCode))
		if statusCode >= 400 {
			attrs = append(attrs, attribute.String("error.type", "http_error"))
			span.SetStatus(codes.Error, fmt.Sprintf("HTTP %d", statusCode))
		}
	}

	measureOpt := metric.WithAttributes(attrs...)
	c.counter.Add(ctx, 1, measureOpt)
	c.histogram.Record(ctx, elapsed, measureOpt)

	// Record the per-phase timings on the span and as metrics. They are
	// captured even on transport errors (e.g. a DNS failure still yields a
	// dns timing of ~0), so always attach what we have.
	span.SetAttributes(
		attribute.Float64("dns.duration_s", timings.dns),
		attribute.Float64("connect.duration_s", timings.connect),
		attribute.Float64("tls.duration_s", timings.tls),
		attribute.Float64("ttfb.duration_s", timings.ttfb),
	)
	c.phases.dns.Record(ctx, timings.dns, measureOpt)
	c.phases.connect.Record(ctx, timings.connect, measureOpt)
	c.phases.tls.Record(ctx, timings.tls, measureOpt)
	c.phases.ttfb.Record(ctx, timings.ttfb, measureOpt)

	if err != nil {
		c.logger.ErrorContext(ctx, "request failed",
			"url", c.url,
			"duration_s", elapsed,
			"dns_s", timings.dns,
			"connect_s", timings.connect,
			"tls_s", timings.tls,
			"ttfb_s", timings.ttfb,
			"error", err,
		)
		return
	}

	if statusCode >= 400 {
		c.logger.ErrorContext(ctx, "request failed",
			"url", c.url,
			"status_code", statusCode,
			"duration_s", elapsed,
			"dns_s", timings.dns,
			"connect_s", timings.connect,
			"tls_s", timings.tls,
			"ttfb_s", timings.ttfb,
		)
		return
	}

	c.logger.InfoContext(ctx, "request completed",
		"url", c.url,
		"status_code", statusCode,
		"duration_s", elapsed,
		"dns_s", timings.dns,
		"connect_s", timings.connect,
		"tls_s", timings.tls,
		"ttfb_s", timings.ttfb,
	)
}

// fetch issues the GET request and drains/closes the body. It returns the HTTP
// status code and the per-phase network timings. A net/http/httptrace hook set
// records the DNS, connect, TLS and time-to-first-byte durations.
func (c *testClient) fetch(ctx context.Context) (int, requestTimings, error) {
	var (
		timings                                      requestTimings
		dnsStart, connectStart, tlsStart, wroteReqAt time.Time
	)
	trace := &httptrace.ClientTrace{
		DNSStart: func(httptrace.DNSStartInfo) { dnsStart = time.Now() },
		DNSDone: func(httptrace.DNSDoneInfo) {
			if !dnsStart.IsZero() {
				timings.dns = time.Since(dnsStart).Seconds()
			}
		},
		ConnectStart: func(_, _ string) { connectStart = time.Now() },
		ConnectDone: func(_, _ string, _ error) {
			if !connectStart.IsZero() {
				timings.connect = time.Since(connectStart).Seconds()
			}
		},
		TLSHandshakeStart: func() { tlsStart = time.Now() },
		TLSHandshakeDone: func(tls.ConnectionState, error) {
			if !tlsStart.IsZero() {
				timings.tls = time.Since(tlsStart).Seconds()
			}
		},
		WroteRequest: func(httptrace.WroteRequestInfo) { wroteReqAt = time.Now() },
		GotFirstResponseByte: func() {
			if !wroteReqAt.IsZero() {
				timings.ttfb = time.Since(wroteReqAt).Seconds()
			}
		},
	}
	ctx = httptrace.WithClientTrace(ctx, trace)

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.url, nil)
	if err != nil {
		return 0, timings, fmt.Errorf("build request: %w", err)
	}
	otel.GetTextMapPropagator().Inject(ctx, propagation.HeaderCarrier(req.Header))

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return 0, timings, fmt.Errorf("do request: %w", err)
	}
	defer resp.Body.Close()
	// Drain the body so the full response time is reflected in the timings.
	_, _ = io.Copy(io.Discard, resp.Body)

	return resp.StatusCode, timings, nil
}

// setupOTel configures the global tracer, meter and logger providers using the
// autoexport package, which reads the standard OTEL_* environment variables to
// select the OTLP protocol and endpoints. It returns a single shutdown function
// that flushes and stops all three providers.
func setupOTel(ctx context.Context) (func(context.Context) error, error) {
	if os.Getenv("OTEL_EXPORTER_OTLP_ENDPOINT") == "" {
		_ = os.Setenv("OTEL_EXPORTER_OTLP_ENDPOINT", defaultOTLPEndpoint)
	}
	if os.Getenv("OTEL_EXPORTER_OTLP_PROTOCOL") == "" {
		_ = os.Setenv("OTEL_EXPORTER_OTLP_PROTOCOL", defaultOTLPProtocol)
	}
	res, err := newResource(ctx)
	if err != nil {
		return nil, err
	}

	otel.SetTextMapPropagator(propagation.NewCompositeTextMapPropagator(
		propagation.TraceContext{},
		propagation.Baggage{},
	))

	var shutdownFuncs []func(context.Context) error
	shutdown := func(ctx context.Context) error {
		var errs error
		for _, fn := range shutdownFuncs {
			errs = errors.Join(errs, fn(ctx))
		}
		shutdownFuncs = nil
		return errs
	}

	// Traces.
	spanExporter, err := autoexport.NewSpanExporter(ctx)
	if err != nil {
		return nil, fmt.Errorf("create span exporter: %w", err)
	}
	tracerProvider := sdktrace.NewTracerProvider(
		sdktrace.WithResource(res),
		sdktrace.WithBatcher(spanExporter),
	)
	shutdownFuncs = append(shutdownFuncs, tracerProvider.Shutdown)
	otel.SetTracerProvider(tracerProvider)

	// Metrics.
	metricReader, err := autoexport.NewMetricReader(ctx)
	if err != nil {
		return nil, fmt.Errorf("create metric reader: %w", err)
	}
	// Override the SDK default bucket boundaries (0..10000) for every
	// test.client.*.duration histogram. Durations are in seconds and typically
	// well under a second, so the buckets are tuned for the 1ms..5s range.
	durationView := sdkmetric.NewView(
		sdkmetric.Instrument{Name: "test.client.*.duration"},
		sdkmetric.Stream{
			Aggregation: sdkmetric.AggregationExplicitBucketHistogram{
				Boundaries: durationBoundaries,
			},
		},
	)
	meterProvider := sdkmetric.NewMeterProvider(
		sdkmetric.WithResource(res),
		sdkmetric.WithReader(metricReader),
		sdkmetric.WithView(durationView),
	)
	shutdownFuncs = append(shutdownFuncs, meterProvider.Shutdown)
	otel.SetMeterProvider(meterProvider)

	// Logs.
	logExporter, err := autoexport.NewLogExporter(ctx)
	if err != nil {
		return nil, fmt.Errorf("create log exporter: %w", err)
	}
	loggerProvider := sdklog.NewLoggerProvider(
		sdklog.WithResource(res),
		sdklog.WithProcessor(sdklog.NewBatchProcessor(logExporter)),
	)
	shutdownFuncs = append(shutdownFuncs, loggerProvider.Shutdown)
	otellog.SetLoggerProvider(loggerProvider)

	// Route slog through both stderr (for local visibility) and the OTel log
	// pipeline (for OTLP export).
	otelHandler := otelslog.NewHandler(instrumentationName, otelslog.WithLoggerProvider(loggerProvider))
	stderrHandler := slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelInfo})
	slog.SetDefault(slog.New(fanoutHandler{handlers: []slog.Handler{stderrHandler, otelHandler}}))

	return shutdown, nil
}

// newResource builds the resource describing this service. Service name comes
// from OTEL_SERVICE_NAME (handled by the SDK) with a code-level default.
func newResource(ctx context.Context) (*resource.Resource, error) {
	if os.Getenv("OTEL_SERVICE_NAME") == "" {
		// Provide a sensible default without overriding an explicit setting.
		_ = os.Setenv("OTEL_SERVICE_NAME", defaultClientName)
	}
	res, err := resource.New(ctx,
		resource.WithAttributes(attribute.String("client", defaultClientName)),
		resource.WithFromEnv(),
		resource.WithTelemetrySDK(),
		resource.WithHost(),
		resource.WithProcessRuntimeDescription(),
	)
	if err != nil {
		return nil, fmt.Errorf("create resource: %w", err)
	}
	return res, nil
}

// fanoutHandler dispatches slog records to multiple underlying handlers.
type fanoutHandler struct {
	handlers []slog.Handler
}

func (h fanoutHandler) Enabled(ctx context.Context, level slog.Level) bool {
	for _, inner := range h.handlers {
		if inner.Enabled(ctx, level) {
			return true
		}
	}
	return false
}

func (h fanoutHandler) Handle(ctx context.Context, record slog.Record) error {
	var errs error
	for _, inner := range h.handlers {
		if inner.Enabled(ctx, record.Level) {
			errs = errors.Join(errs, inner.Handle(ctx, record.Clone()))
		}
	}
	return errs
}

func (h fanoutHandler) WithAttrs(attrs []slog.Attr) slog.Handler {
	next := make([]slog.Handler, len(h.handlers))
	for i, inner := range h.handlers {
		next[i] = inner.WithAttrs(attrs)
	}
	return fanoutHandler{handlers: next}
}

func (h fanoutHandler) WithGroup(name string) slog.Handler {
	next := make([]slog.Handler, len(h.handlers))
	for i, inner := range h.handlers {
		next[i] = inner.WithGroup(name)
	}
	return fanoutHandler{handlers: next}
}
