// otel-http-client is a minimal OpenTelemetry-instrumented HTTP client.
//
// It performs a GET against TEST_URL on a fixed interval (TEST_PERIOD) and emits
// traces, metrics (request counter + latency histogram) and logs over OTLP. All
// exporter configuration is taken from the standard OTEL_* environment variables
// (read by the opentelemetry-cpp OTLP HTTP exporter options).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <curl/curl.h>

#include "opentelemetry/baggage/propagation/baggage_propagator.h"
#include "opentelemetry/context/context.h"
#include "opentelemetry/context/propagation/composite_propagator.h"
#include "opentelemetry/context/propagation/global_propagator.h"
#include "opentelemetry/context/propagation/text_map_propagator.h"
#include "opentelemetry/context/runtime_context.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_http_log_record_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_log_record_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h"
#include "opentelemetry/logs/logger.h"
#include "opentelemetry/logs/logger_provider.h"
#include "opentelemetry/logs/provider.h"
#include "opentelemetry/metrics/provider.h"
#include "opentelemetry/sdk/logs/batch_log_record_processor_factory.h"
#include "opentelemetry/sdk/logs/batch_log_record_processor_options.h"
#include "opentelemetry/sdk/logs/logger_provider.h"
#include "opentelemetry/sdk/logs/logger_provider_factory.h"
#include "opentelemetry/sdk/metrics/aggregation/aggregation_config.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/meter_provider.h"
#include "opentelemetry/sdk/metrics/meter_provider_factory.h"
#include "opentelemetry/sdk/metrics/view/instrument_selector_factory.h"
#include "opentelemetry/sdk/metrics/view/meter_selector_factory.h"
#include "opentelemetry/sdk/metrics/view/view_factory.h"
#include "opentelemetry/sdk/metrics/view/view_registry_factory.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/sdk/trace/batch_span_processor_factory.h"
#include "opentelemetry/sdk/trace/batch_span_processor_options.h"
#include "opentelemetry/sdk/trace/tracer_provider.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/trace/propagation/http_trace_context.h"
#include "opentelemetry/trace/provider.h"

namespace otlp = opentelemetry::exporter::otlp;
namespace trace_api = opentelemetry::trace;
namespace trace_sdk = opentelemetry::sdk::trace;
namespace metrics_api = opentelemetry::metrics;
namespace metrics_sdk = opentelemetry::sdk::metrics;
namespace logs_api = opentelemetry::logs;
namespace logs_sdk = opentelemetry::sdk::logs;
namespace resource = opentelemetry::sdk::resource;
namespace propagation = opentelemetry::context::propagation;
namespace nostd = opentelemetry::nostd;

namespace {

constexpr const char *kInstrumentationName = "otel-tracing";
constexpr const char *kDefaultClientName = "cpp-client";
constexpr const char *kDefaultTestURL = "https://example.com/";
constexpr const char *kDefaultOTLPEndpoint = "http://localhost:4318";
constexpr const char *kDefaultOTLPProtocol = "http/protobuf";
constexpr std::chrono::milliseconds kDefaultPeriod{1000};

// Set by the signal handler to request a graceful shutdown.
std::atomic<bool> g_stop{false};

void HandleSignal(int /*signum*/) { g_stop.store(true); }

std::string GetEnv(const char *name, const std::string &fallback) {
  const char *value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  return std::string(value);
}

// ParsePeriod parses a Go-style duration string (e.g. "1s", "500ms", "2m").
// Supported units: ms, s, m, h. Returns false on an invalid or non-positive
// value.
bool ParsePeriod(const std::string &text, std::chrono::milliseconds &out) {
  std::size_t idx = 0;
  double value = 0.0;
  try {
    value = std::stod(text, &idx);
  } catch (const std::exception &) {
    return false;
  }
  const std::string unit = text.substr(idx);
  double millis = 0.0;
  if (unit == "ms") {
    millis = value;
  } else if (unit == "s" || unit.empty()) {
    millis = value * 1000.0;
  } else if (unit == "m") {
    millis = value * 60.0 * 1000.0;
  } else if (unit == "h") {
    millis = value * 60.0 * 60.0 * 1000.0;
  } else {
    return false;
  }
  if (millis <= 0.0) {
    return false;
  }
  out = std::chrono::milliseconds(static_cast<std::int64_t>(millis));
  return true;
}

std::string FormatPeriod(std::chrono::milliseconds period) {
  if (period.count() % 1000 == 0) {
    return std::to_string(period.count() / 1000) + "s";
  }
  return std::to_string(period.count()) + "ms";
}

std::string RuntimeDescription() {
  return std::string("cpp ") + __VERSION__;
}

std::string NowTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  return std::string(buf);
}

// HttpHeaderCarrier collects headers written by the propagator so they can be
// attached to the outgoing curl request.
class HttpHeaderCarrier : public propagation::TextMapCarrier {
 public:
  nostd::string_view Get(nostd::string_view /*key*/) const noexcept override {
    return "";
  }
  void Set(nostd::string_view key, nostd::string_view value) noexcept override {
    headers[std::string(key)] = std::string(value);
  }
  std::map<std::string, std::string> headers;
};

size_t DiscardBody(char * /*ptr*/, size_t size, size_t nmemb, void * /*userdata*/) {
  return size * nmemb;
}

// Telemetry owns the SDK providers and flushes/shuts them down on destruction.
class Telemetry {
 public:
  void Init(const resource::Resource &res) {
    propagation::GlobalTextMapPropagator::SetGlobalPropagator(
        nostd::shared_ptr<propagation::TextMapPropagator>(MakeCompositePropagator()));

    InitTracer(res);
    InitMeter(res);
    InitLogger(res);
  }

  void Shutdown() {
    if (tracer_provider_) {
      auto *p = static_cast<trace_sdk::TracerProvider *>(tracer_provider_.get());
      p->ForceFlush();
      p->Shutdown();
    }
    if (meter_provider_) {
      auto *p = static_cast<metrics_sdk::MeterProvider *>(meter_provider_.get());
      p->ForceFlush();
      p->Shutdown();
    }
    if (logger_provider_) {
      auto *p = static_cast<logs_sdk::LoggerProvider *>(logger_provider_.get());
      p->ForceFlush();
      p->Shutdown();
    }
    trace_api::Provider::SetTracerProvider(
        nostd::shared_ptr<trace_api::TracerProvider>());
    metrics_api::Provider::SetMeterProvider(
        nostd::shared_ptr<metrics_api::MeterProvider>());
    logs_api::Provider::SetLoggerProvider(
        nostd::shared_ptr<logs_api::LoggerProvider>());
  }

 private:
  static propagation::TextMapPropagator *MakeCompositePropagator() {
    std::vector<std::unique_ptr<propagation::TextMapPropagator>> propagators;
    propagators.emplace_back(new trace_api::propagation::HttpTraceContext());
    propagators.emplace_back(
        new opentelemetry::baggage::propagation::BaggagePropagator());
    return new propagation::CompositePropagator(std::move(propagators));
  }

  void InitTracer(const resource::Resource &res) {
    otlp::OtlpHttpExporterOptions opts;  // reads OTEL_EXPORTER_OTLP_* env vars
    auto exporter = otlp::OtlpHttpExporterFactory::Create(opts);
    trace_sdk::BatchSpanProcessorOptions proc_opts;
    auto processor =
        trace_sdk::BatchSpanProcessorFactory::Create(std::move(exporter), proc_opts);
    tracer_provider_ = trace_sdk::TracerProviderFactory::Create(std::move(processor), res);
    trace_api::Provider::SetTracerProvider(tracer_provider_);
  }

  void InitMeter(const resource::Resource &res) {
    otlp::OtlpHttpMetricExporterOptions opts;
    auto exporter = otlp::OtlpHttpMetricExporterFactory::Create(opts);
    metrics_sdk::PeriodicExportingMetricReaderOptions reader_opts;
    auto reader = metrics_sdk::PeriodicExportingMetricReaderFactory::Create(
        std::move(exporter), reader_opts);

    auto views = metrics_sdk::ViewRegistryFactory::Create();
    // Override the SDK default bucket boundaries (0..10000) for every
    // test.client.*.duration histogram. Durations are in seconds and typically
    // well under a second, so the buckets are tuned for the 1ms..5s range.
    const std::vector<double> boundaries = {0.001, 0.0025, 0.005, 0.01, 0.025,
                                            0.05,  0.075,  0.1,   0.25, 0.5,
                                            0.75,  1.0,    2.5,   5.0};
    const std::vector<std::pair<std::string, std::string>> duration_histograms = {
        {"test.client.request.duration",
         "Total duration of HTTP requests executed by the test client"},
        {"test.client.dns.duration", "DNS resolution time"},
        {"test.client.connect.duration", "TCP connection establishment time"},
        {"test.client.tls.duration", "TLS handshake time"},
        {"test.client.ttfb.duration",
         "Time to first response byte (server wait time)"},
    };
    for (const auto &hist : duration_histograms) {
      auto config = std::make_shared<metrics_sdk::HistogramAggregationConfig>();
      config->boundaries_ = boundaries;
      auto instrument_selector = metrics_sdk::InstrumentSelectorFactory::Create(
          metrics_sdk::InstrumentType::kHistogram, hist.first, "s");
      auto meter_selector =
          metrics_sdk::MeterSelectorFactory::Create(kInstrumentationName, "", "");
      auto view = metrics_sdk::ViewFactory::Create(
          hist.first, hist.second, "s", metrics_sdk::AggregationType::kHistogram,
          config);
      views->AddView(std::move(instrument_selector), std::move(meter_selector),
                     std::move(view));
    }

    meter_provider_ = metrics_sdk::MeterProviderFactory::Create(std::move(views), res);
    auto *sdk_provider = static_cast<metrics_sdk::MeterProvider *>(meter_provider_.get());
    sdk_provider->AddMetricReader(std::move(reader));
    metrics_api::Provider::SetMeterProvider(meter_provider_);
  }

  void InitLogger(const resource::Resource &res) {
    otlp::OtlpHttpLogRecordExporterOptions opts;
    auto exporter = otlp::OtlpHttpLogRecordExporterFactory::Create(opts);
    logs_sdk::BatchLogRecordProcessorOptions proc_opts;
    auto processor = logs_sdk::BatchLogRecordProcessorFactory::Create(
        std::move(exporter), proc_opts);
    logger_provider_ = logs_sdk::LoggerProviderFactory::Create(std::move(processor), res);
    logs_api::Provider::SetLoggerProvider(logger_provider_);
  }

  nostd::shared_ptr<trace_api::TracerProvider> tracer_provider_;
  nostd::shared_ptr<metrics_api::MeterProvider> meter_provider_;
  nostd::shared_ptr<logs_api::LoggerProvider> logger_provider_;
};

// RequestTimings holds the per-phase durations of a single request, in seconds.
struct RequestTimings {
  double dns = 0.0;
  double connect = 0.0;
  double tls = 0.0;
  double ttfb = 0.0;
};

// PhaseHistograms bundles the DNS / connect / TLS / TTFB latency instruments.
struct PhaseHistograms {
  opentelemetry::metrics::Histogram<double> *dns = nullptr;
  opentelemetry::metrics::Histogram<double> *connect = nullptr;
  opentelemetry::metrics::Histogram<double> *tls = nullptr;
  opentelemetry::metrics::Histogram<double> *ttfb = nullptr;
};

// TestClient executes and instruments a single GET request per call.
class TestClient {
 public:
  TestClient(std::string url,
             nostd::shared_ptr<trace_api::Tracer> tracer,
             nostd::shared_ptr<logs_api::Logger> logger,
             opentelemetry::metrics::Counter<std::uint64_t> *counter,
             opentelemetry::metrics::Histogram<double> *histogram,
             PhaseHistograms phases)
      : url_(std::move(url)),
        tracer_(std::move(tracer)),
        logger_(std::move(logger)),
        counter_(counter),
        histogram_(histogram),
        phases_(phases) {}

  void DoRequest() {
    trace_api::StartSpanOptions options;
    options.kind = trace_api::SpanKind::kClient;
    auto span = tracer_->StartSpan("test-request", {}, options);
    auto scope = trace_api::Tracer::WithActiveSpan(span);

    const auto start = std::chrono::steady_clock::now();
    long status_code = 0;
    std::string error;
    RequestTimings timings;
    const bool ok = Fetch(status_code, error, timings);
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::map<std::string, opentelemetry::common::AttributeValue> attrs;
    attrs["http.request.method"] = "GET";
    attrs["url.full"] = url_.c_str();
    if (ok) {
      attrs["http.response.status_code"] = static_cast<std::int64_t>(status_code);
      span->SetAttribute("http.response.status_code",
                         static_cast<std::int64_t>(status_code));
      if (status_code >= 400) {
        attrs["error.type"] = "http_error";
        span->SetAttribute("error.type", "http_error");
        span->SetStatus(trace_api::StatusCode::kError,
                        "HTTP " + std::to_string(status_code));
      }
    } else {
      attrs["error.type"] = "request_error";
      span->SetAttribute("error.type", "request_error");
      span->SetStatus(trace_api::StatusCode::kError, error);
    }
    span->SetAttribute("http.request.method", "GET");
    span->SetAttribute("url.full", url_.c_str());

    // Per-phase network timings on the span and as metrics.
    span->SetAttribute("dns.duration_s", timings.dns);
    span->SetAttribute("connect.duration_s", timings.connect);
    span->SetAttribute("tls.duration_s", timings.tls);
    span->SetAttribute("ttfb.duration_s", timings.ttfb);

    const opentelemetry::context::Context ctx{};
    counter_->Add(1, attrs, ctx);
    histogram_->Record(elapsed, attrs, ctx);
    phases_.dns->Record(timings.dns, attrs, ctx);
    phases_.connect->Record(timings.connect, attrs, ctx);
    phases_.tls->Record(timings.tls, attrs, ctx);
    phases_.ttfb->Record(timings.ttfb, attrs, ctx);

    if (!ok) {
      Log(logs_api::Severity::kError, "request failed",
          {{"url", url_},
           {"duration_s", std::to_string(elapsed)},
           {"dns_s", std::to_string(timings.dns)},
           {"connect_s", std::to_string(timings.connect)},
           {"tls_s", std::to_string(timings.tls)},
           {"ttfb_s", std::to_string(timings.ttfb)},
           {"error", error}});
    } else if (status_code >= 400) {
      Log(logs_api::Severity::kError, "request failed",
          {{"url", url_},
           {"status_code", std::to_string(status_code)},
           {"duration_s", std::to_string(elapsed)},
           {"dns_s", std::to_string(timings.dns)},
           {"connect_s", std::to_string(timings.connect)},
           {"tls_s", std::to_string(timings.tls)},
           {"ttfb_s", std::to_string(timings.ttfb)}});
    } else {
      Log(logs_api::Severity::kInfo, "request completed",
          {{"url", url_},
           {"status_code", std::to_string(status_code)},
           {"duration_s", std::to_string(elapsed)},
           {"dns_s", std::to_string(timings.dns)},
           {"connect_s", std::to_string(timings.connect)},
           {"tls_s", std::to_string(timings.tls)},
           {"ttfb_s", std::to_string(timings.ttfb)}});
    }

    span->End();
  }

  void Log(logs_api::Severity severity, const std::string &message,
           const std::vector<std::pair<std::string, std::string>> &fields) {
    std::string level = severity == logs_api::Severity::kError ? "ERROR" : "INFO";
    std::cerr << "time=" << NowTimestamp() << " level=" << level
              << " msg=\"" << message << "\"";
    for (const auto &kv : fields) {
      std::cerr << " " << kv.first << "=" << kv.second;
    }
    std::cerr << "\n";

    auto record = logger_->CreateLogRecord();
    record->SetSeverity(severity);
    record->SetBody(message);
    for (const auto &kv : fields) {
      record->SetAttribute(kv.first, kv.second.c_str());
    }
    logger_->EmitLogRecord(std::move(record));
  }

 private:
  bool Fetch(long &status_code, std::string &error, RequestTimings &timings) {
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
      error = "curl_easy_init failed";
      return false;
    }

    // Inject the active trace context into request headers (W3C traceparent).
    HttpHeaderCarrier carrier;
    auto current = opentelemetry::context::RuntimeContext::GetCurrent();
    propagation::GlobalTextMapPropagator::GetGlobalPropagator()->Inject(carrier, current);

    struct curl_slist *header_list = nullptr;
    for (const auto &kv : carrier.headers) {
      header_list =
          curl_slist_append(header_list, (kv.first + ": " + kv.second).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, DiscardBody);
    if (header_list != nullptr) {
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    const CURLcode res = curl_easy_perform(curl);
    bool ok = false;
    if (res != CURLE_OK) {
      error = curl_easy_strerror(res);
    } else {
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
      ok = true;
    }

    // curl exposes cumulative timestamps (microseconds) measured from the start
    // of the request. Convert them into per-phase durations.
    curl_off_t namelookup = 0, connect_t = 0, appconnect = 0, pretransfer = 0,
               starttransfer = 0;
    curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME_T, &namelookup);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME_T, &connect_t);
    curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME_T, &appconnect);
    curl_easy_getinfo(curl, CURLINFO_PRETRANSFER_TIME_T, &pretransfer);
    curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME_T, &starttransfer);

    constexpr double kUsToS = 1e6;
    timings.dns = static_cast<double>(namelookup) / kUsToS;
    timings.connect =
        connect_t > namelookup ? static_cast<double>(connect_t - namelookup) / kUsToS : 0.0;
    timings.tls =
        appconnect > connect_t ? static_cast<double>(appconnect - connect_t) / kUsToS : 0.0;
    const curl_off_t wait_base = pretransfer > 0 ? pretransfer : connect_t;
    timings.ttfb = starttransfer > wait_base
                       ? static_cast<double>(starttransfer - wait_base) / kUsToS
                       : 0.0;

    if (header_list != nullptr) {
      curl_slist_free_all(header_list);
    }
    curl_easy_cleanup(curl);
    return ok;
  }

  std::string url_;
  nostd::shared_ptr<trace_api::Tracer> tracer_;
  nostd::shared_ptr<logs_api::Logger> logger_;
  opentelemetry::metrics::Counter<std::uint64_t> *counter_;
  opentelemetry::metrics::Histogram<double> *histogram_;
  PhaseHistograms phases_;
};

resource::Resource MakeResource() {
  // Provide a default service name without overriding an explicit setting.
  if (std::getenv("OTEL_SERVICE_NAME") == nullptr) {
    setenv("OTEL_SERVICE_NAME", kDefaultClientName, 0);
  }
  // Resource::Create merges OTEL_RESOURCE_ATTRIBUTES, OTEL_SERVICE_NAME, and SDK
  // attributes. Host and runtime attributes mirror Go's WithHost() and
  // WithProcessRuntimeDescription().
  resource::ResourceAttributes attributes{{"client", kDefaultClientName}};
  auto res = resource::Resource::Create(attributes);

  char hostname[256] = {};
  if (gethostname(hostname, sizeof(hostname) - 1) == 0) {
    res = resource::Resource::Create({{"host.name", hostname}}).Merge(res);
  }
  res = resource::Resource::Create({{"process.runtime.description", RuntimeDescription()}})
            .Merge(res);
  return res;
}

}  // namespace

int main() {
  const std::string test_url = GetEnv("TEST_URL", kDefaultTestURL);

  std::chrono::milliseconds period = kDefaultPeriod;
  const std::string period_env = GetEnv("TEST_PERIOD", "");
  if (!period_env.empty() && !ParsePeriod(period_env, period)) {
    std::cerr << "invalid TEST_PERIOD: " << period_env << "\n";
    return 1;
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  if (std::getenv("OTEL_EXPORTER_OTLP_ENDPOINT") == nullptr) {
    setenv("OTEL_EXPORTER_OTLP_ENDPOINT", kDefaultOTLPEndpoint, 0);
  }
  if (std::getenv("OTEL_EXPORTER_OTLP_PROTOCOL") == nullptr) {
    setenv("OTEL_EXPORTER_OTLP_PROTOCOL", kDefaultOTLPProtocol, 0);
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);

  Telemetry telemetry;
  telemetry.Init(MakeResource());

  auto tracer =
      trace_api::Provider::GetTracerProvider()->GetTracer(kInstrumentationName);
  auto logger = logs_api::Provider::GetLoggerProvider()->GetLogger(
      kInstrumentationName, kInstrumentationName);

  auto meter = metrics_api::Provider::GetMeterProvider()->GetMeter(kInstrumentationName);
  // Names are prefixed with test.client to mirror the Go example.
  auto counter = meter->CreateUInt64Counter(
      "test.client.requests",
      "Number of HTTP requests executed by the test client", "{request}");
  auto histogram = meter->CreateDoubleHistogram(
      "test.client.request.duration",
      "Total duration of HTTP requests executed by the test client", "s");
  auto dns_histogram = meter->CreateDoubleHistogram(
      "test.client.dns.duration", "DNS resolution time", "s");
  auto connect_histogram = meter->CreateDoubleHistogram(
      "test.client.connect.duration", "TCP connection establishment time", "s");
  auto tls_histogram = meter->CreateDoubleHistogram(
      "test.client.tls.duration", "TLS handshake time", "s");
  auto ttfb_histogram = meter->CreateDoubleHistogram(
      "test.client.ttfb.duration", "Time to first response byte (server wait time)", "s");

  PhaseHistograms phases;
  phases.dns = dns_histogram.get();
  phases.connect = connect_histogram.get();
  phases.tls = tls_histogram.get();
  phases.ttfb = ttfb_histogram.get();

  TestClient client(test_url, tracer, logger, counter.get(), histogram.get(), phases);

  client.Log(logs_api::Severity::kInfo, "starting test client",
             {{"url", test_url}, {"period", FormatPeriod(period)}});

  // Fire one request immediately, then on every tick.
  client.DoRequest();

  while (!g_stop.load()) {
    const auto deadline = std::chrono::steady_clock::now() + period;
    while (!g_stop.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (g_stop.load()) {
      break;
    }
    client.DoRequest();
  }

  client.Log(logs_api::Severity::kInfo, "shutting down", {});

  telemetry.Shutdown();
  curl_global_cleanup();
  return 0;
}
