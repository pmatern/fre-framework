#include <fre/service/harness.hpp>
#include <fre/pipeline/pipeline_config.hpp>

#include <catch2/catch_test_macros.hpp>

#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include <chrono>
#include <sstream>
#include <string>
#include <thread>

using namespace std::chrono_literals;

// ─── Helpers ──────────────────────────────────────────────────────────────────

namespace {

/// Send a raw HTTP request to localhost:port and return the full response string.
std::string http_request(uint16_t port, const std::string& request) {
    asio::io_context ioc;
    asio::ip::tcp::socket sock{ioc};
    asio::ip::tcp::resolver resolver{ioc};

    asio::error_code ec;
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port), ec);
    if (ec) return "";

    asio::connect(sock, endpoints, ec);
    if (ec) return "";

    asio::write(sock, asio::buffer(request), ec);
    if (ec) return "";

    asio::streambuf buf;
    asio::read_until(sock, buf, "\r\n\r\n", ec);

    std::string response{
        asio::buffers_begin(buf.data()),
        asio::buffers_end(buf.data())};

    // Also read any body
    std::string extra;
    while (true) {
        std::size_t n = asio::read(sock, buf, asio::transfer_at_least(1), ec);
        if (ec || n == 0) break;
        extra += std::string{
            asio::buffers_begin(buf.data()),
            asio::buffers_end(buf.data())};
        buf.consume(buf.size());
    }
    return response + extra;
}

/// Extract status code from an HTTP response string.
int status_code(const std::string& response) {
    if (response.size() < 12) return 0;
    return std::stoi(response.substr(9, 3));
}

}  // namespace

// ─── Tests ───────────────────────────────────────────────────────────────────

SCENARIO("ServiceHarness exposes HTTP endpoints for a minimal pipeline",
         "[integration][service_harness]")
{
    constexpr uint16_t TEST_PORT = 18080;

    GIVEN("a service harness with a minimal pipeline on port 18080") {
        auto config_result = fre::PipelineConfig::Builder{}
            .pipeline_id("test-service")
            .emit_config([]{
                fre::EmitStageConfig c;
                c.add_stdout_target();
                return c;
            }())
            .build();
        REQUIRE(config_result.has_value());

        fre::service::HarnessConfig harness_cfg;
        harness_cfg.bind_address    = "127.0.0.1";
        harness_cfg.port            = TEST_PORT;
        harness_cfg.pipeline_config = std::move(*config_result);

        fre::service::ServiceHarness harness{std::move(harness_cfg)};
        REQUIRE(harness.start());

        // Allow the acceptor thread to start
        std::this_thread::sleep_for(50ms);

        WHEN("POST /events is sent with a valid JSON event") {
            const std::string body     = R"({"tenant_id":"t1","entity_id":"e1","event_type":"test"})";
            const std::string request  = "POST /events HTTP/1.1\r\n"
                                         "Host: localhost\r\n"
                                         "Content-Type: application/json\r\n"
                                         "Content-Length: " + std::to_string(body.size()) + "\r\n"
                                         "Connection: close\r\n"
                                         "\r\n" + body;

            auto response = http_request(TEST_PORT, request);

            THEN("response is 202 Accepted") {
                REQUIRE(status_code(response) == 202);
            }
        }

        WHEN("GET /health is requested") {
            const std::string request = "GET /health HTTP/1.1\r\n"
                                        "Host: localhost\r\n"
                                        "Connection: close\r\n"
                                        "\r\n";

            auto response = http_request(TEST_PORT, request);

            THEN("response is 200 with JSON body containing 'state' and 'degraded'") {
                REQUIRE(status_code(response) == 200);
                REQUIRE(response.find("\"state\"") != std::string::npos);
                REQUIRE(response.find("\"degraded\"") != std::string::npos);
            }
        }

        WHEN("POST /pipeline/drain is requested") {
            const std::string request = "POST /pipeline/drain HTTP/1.1\r\n"
                                        "Host: localhost\r\n"
                                        "Content-Length: 0\r\n"
                                        "Connection: close\r\n"
                                        "\r\n";

            auto response = http_request(TEST_PORT, request);

            THEN("response is 200 and pipeline reports stopped") {
                REQUIRE(status_code(response) == 200);
                REQUIRE(response.find("drained") != std::string::npos);
            }
        }

        harness.stop();
    }
}
