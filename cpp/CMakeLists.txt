cmake_minimum_required(VERSION 3.16)
project(otel_http_client CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
# Transitive dependencies referenced by the opentelemetry-cpp exported targets.
# They must be found before opentelemetry-cpp so its IMPORTED targets resolve.
find_package(absl CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Protobuf REQUIRED)
find_package(opentelemetry-cpp CONFIG REQUIRED)

add_executable(otel-http-client main.cpp)

target_include_directories(otel-http-client PRIVATE ${OPENTELEMETRY_CPP_INCLUDE_DIRS})

target_link_libraries(otel-http-client PRIVATE
  opentelemetry-cpp::api
  opentelemetry-cpp::sdk
  opentelemetry-cpp::common
  opentelemetry-cpp::resources
  opentelemetry-cpp::trace
  opentelemetry-cpp::metrics
  opentelemetry-cpp::logs
  opentelemetry-cpp::otlp_http_exporter
  opentelemetry-cpp::otlp_http_metric_exporter
  opentelemetry-cpp::otlp_http_log_record_exporter
  CURL::libcurl
  Threads::Threads
)
