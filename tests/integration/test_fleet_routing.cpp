/// Integration tests for fleet-level shuffle shard routing via the ServiceHarness.
///
/// Two harness instances are started in-process on different ports with
/// fleet_size=2, instances_per_tenant=1.  Each owns a disjoint tenant set.
/// We verify that the non-owner returns HTTP 503 + X-Fre-Redirect-Hint and
/// the owner returns HTTP 202.

#include <fre/service/harness.hpp>
#include <fre/service/fleet_router.hpp>
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
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace fre::service;

// ─── HTTP helpers ─────────────────────────────────────────────────────────────

namespace {

struct HttpResponse {
    int         status{0};
    std::string headers;
    std::string body;
};

HttpResponse http_post(uint16_t port, const std::string& path, const std::string& body) {
    asio::io_context ioc;
    asio::ip::tcp::socket sock{ioc};
    asio::ip::tcp::resolver resolver{ioc};

    asio::error_code ec;
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port), ec);
    if (ec) return {};
    asio::connect(sock, endpoints, ec);
    if (ec) return {};

    const std::string request =
        "POST " + path + " HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + body;

    asio::write(sock, asio::buffer(request), ec);
    if (ec) return {};

    asio::streambuf buf;
    asio::read_until(sock, buf, "\r\n\r\n", ec);

    HttpResponse resp;
    resp.headers = std::string{asio::buffers_begin(buf.data()), asio::buffers_end(buf.data())};
    buf.consume(buf.size());

    // Parse status line
    if (resp.headers.size() >= 12) {
        resp.status = std::stoi(resp.headers.substr(9, 3));
    }

    // Drain body
    std::string extra;
    while (true) {
        std::size_t n = asio::read(sock, buf, asio::transfer_at_least(1), ec);
        if (ec || n == 0) break;
        extra += std::string{asio::buffers_begin(buf.data()), asio::buffers_end(buf.data())};
        buf.consume(buf.size());
    }
    resp.body = extra;
    return resp;
}

HttpResponse http_get(uint16_t port, const std::string& path) {
    asio::io_context ioc;
    asio::ip::tcp::socket sock{ioc};
    asio::ip::tcp::resolver resolver{ioc};

    asio::error_code ec;
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port), ec);
    if (ec) return {};
    asio::connect(sock, endpoints, ec);
    if (ec) return {};

    const std::string request =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n\r\n";

    asio::write(sock, asio::buffer(request), ec);
    if (ec) return {};

    asio::streambuf buf;
    asio::read_until(sock, buf, "\r\n\r\n", ec);
    HttpResponse resp;
    resp.headers = std::string{asio::buffers_begin(buf.data()), asio::buffers_end(buf.data())};
    if (resp.headers.size() >= 12) {
        resp.status = std::stoi(resp.headers.substr(9, 3));
    }

    std::string extra;
    while (true) {
        std::size_t n = asio::read(sock, buf, asio::transfer_at_least(1), ec);
        if (ec || n == 0) break;
        extra += std::string{asio::buffers_begin(buf.data()), asio::buffers_end(buf.data())};
        buf.consume(buf.size());
    }
    resp.body = extra;
    return resp;
}

fre::service::HarnessConfig make_config(uint16_t port, uint32_t instance_id,
                                         uint32_t fleet_size)
{
    auto cfg_result = fre::PipelineConfig::Builder{}
        .pipeline_id("fleet-test-" + std::to_string(instance_id))
        .emit_config([] {
            fre::EmitStageConfig c;
            c.add_noop_target();
            return c;
        }())
        .build();

    fre::service::HarnessConfig hcfg;
    hcfg.bind_address    = "127.0.0.1";
    hcfg.port            = port;
    hcfg.pipeline_config = std::move(*cfg_result);

    FleetConfig fleet;
    fleet.instance_id          = instance_id;
    fleet.fleet_size           = fleet_size;
    fleet.instances_per_tenant = 1; // exactly 1 owner per tenant, disjoint sets
    // Topology so redirect hints have addresses
    fleet.topology.push_back(InstanceInfo{0, "127.0.0.1:19080"});
    fleet.topology.push_back(InstanceInfo{1, "127.0.0.1:19081"});
    hcfg.fleet_config = std::move(fleet);

    return hcfg;
}

// Find a tenant owned by instance `target_id` (not by the other).
std::string tenant_owned_by(uint32_t target_id, uint32_t fleet_size) {
    for (int i = 0; i < 1000; ++i) {
        std::string t = "tenant-" + std::to_string(i);
        FleetRouter r0{make_config(0, 0, fleet_size).fleet_config.value()};
        FleetRouter r1{make_config(0, 1, fleet_size).fleet_config.value()};
        const bool owned_by_0 = r0.owns(t);
        const bool owned_by_1 = r1.owns(t);
        // With instances_per_tenant=1 exactly one of the two instances owns it
        if (target_id == 0 && owned_by_0 && !owned_by_1) return t;
        if (target_id == 1 && owned_by_1 && !owned_by_0) return t;
    }
    return "";
}

}  // namespace

// ─── Tests ────────────────────────────────────────────────────────────────────

SCENARIO("Fleet routing: non-owner returns 503, owner accepts event",
         "[integration][fleet_routing]")
{
    constexpr uint16_t PORT_0 = 19080;
    constexpr uint16_t PORT_1 = 19081;
    constexpr uint32_t FLEET  = 2;

    GIVEN("two service instances with fleet_size=2, instances_per_tenant=1") {
        ServiceHarness harness0{make_config(PORT_0, 0, FLEET)};
        ServiceHarness harness1{make_config(PORT_1, 1, FLEET)};

        REQUIRE(harness0.start());
        REQUIRE(harness1.start());
        std::this_thread::sleep_for(50ms);

        WHEN("a tenant owned by instance 0 is submitted to instance 1 (non-owner)") {
            const std::string tenant = tenant_owned_by(0, FLEET);
            REQUIRE(!tenant.empty());

            const std::string body =
                R"({"tenant_id":")" + tenant + R"(","entity_id":"e1","event_type":"test"})";
            auto resp = http_post(PORT_1, "/events", body);

            THEN("response is 503 with a redirect hint") {
                REQUIRE(resp.status == 503);
                REQUIRE(resp.headers.find("X-Fre-Redirect-Hint") != std::string::npos);
                REQUIRE(resp.headers.find("127.0.0.1:19080") != std::string::npos);
            }
        }

        WHEN("the same tenant is submitted to its owner (instance 0)") {
            const std::string tenant = tenant_owned_by(0, FLEET);
            REQUIRE(!tenant.empty());

            const std::string body =
                R"({"tenant_id":")" + tenant + R"(","entity_id":"e1","event_type":"test"})";
            auto resp = http_post(PORT_0, "/events", body);

            THEN("response is 202 Accepted") {
                REQUIRE(resp.status == 202);
            }
        }

        WHEN("GET /topology is requested on instance 1") {
            auto resp = http_get(PORT_1, "/topology");
            THEN("response is 200 with fleet metadata") {
                REQUIRE(resp.status == 200);
                const auto full = resp.headers + resp.body;
                REQUIRE(full.find("fleet_size") != std::string::npos);
            }
        }

        harness0.stop();
        harness1.stop();
    }
}

SCENARIO("Fleet routing: disabled when fleet_config is absent",
         "[integration][fleet_routing]")
{
    constexpr uint16_t PORT = 19082;

    GIVEN("a service harness with no fleet_config") {
        auto cfg_result = fre::PipelineConfig::Builder{}
            .pipeline_id("no-fleet-test")
            .emit_config([] {
                fre::EmitStageConfig c;
                c.add_noop_target();
                return c;
            }())
            .build();

        fre::service::HarnessConfig hcfg;
        hcfg.bind_address    = "127.0.0.1";
        hcfg.port            = PORT;
        hcfg.pipeline_config = std::move(*cfg_result);
        // fleet_config deliberately left as nullopt

        ServiceHarness harness{std::move(hcfg)};
        REQUIRE(harness.start());
        std::this_thread::sleep_for(50ms);

        WHEN("any tenant submits an event") {
            const std::string body = R"({"tenant_id":"acme","entity_id":"e1","event_type":"test"})";
            auto resp = http_post(PORT, "/events", body);

            THEN("response is 202 (all tenants accepted)") {
                REQUIRE(resp.status == 202);
            }
        }

        WHEN("GET /topology is requested") {
            auto resp = http_get(PORT, "/topology");
            THEN("response is 200 with 'disabled' indicator") {
                REQUIRE(resp.status == 200);
                const auto full = resp.headers + resp.body;
                REQUIRE(full.find("disabled") != std::string::npos);
            }
        }

        harness.stop();
    }
}
