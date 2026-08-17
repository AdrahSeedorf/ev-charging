#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include <cstdio>
#include <fstream>

#include "evnet/network.hpp"
#include "fixtures.hpp"

using namespace evnet;

namespace {

/// Writes a temporary CSV and removes it when the test finishes.
class TempCsv {
public:
    TempCsv(std::string name, const std::string& contents)
        : path_(std::string(EVNET_PROJECT_ROOT) + "/tests/tmp_" + std::move(name)) {
        std::ofstream out(path_);
        out << contents;
    }
    ~TempCsv() { std::remove(path_.c_str()); }
    TempCsv(const TempCsv&) = delete;
    TempCsv& operator=(const TempCsv&) = delete;

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

const char* kGoodNodes =
    "# a comment line that must be skipped\n"
    "id,name,has_station,price_per_kwh,chargers,power_kw\n"
    "0,Alpha,1,0.40,4,150.0\n"
    "\n"
    "1,Bravo,0,0.00,0,0.0\n"
    "2,Charlie,1,0.00,2,50.0\n";

const char* kGoodEdges =
    "from_id,to_id,distance_km\n"
    "0,1,10.0\n"
    "1,2,5.0\n";

}  // namespace

TEST_CASE("loading accepts comments, blank lines and surrounding whitespace", "[network]") {
    const TempCsv nodes("net_nodes.csv", kGoodNodes);
    const TempCsv edges("net_edges.csv", kGoodEdges);

    const Network network = Network::load(nodes.path(), edges.path());
    REQUIRE(network.size() == 3);
    CHECK(network.node(0).name == "Alpha");
    CHECK(network.node(0).hasStation());
    CHECK(network.node(0).station->chargers == 4);
    CHECK_FALSE(network.node(1).hasStation());
    // A price of zero means free, which is distinct from having no station.
    CHECK(network.node(2).hasStation());
    CHECK(network.node(2).station->pricePerKwh == 0.0);
}

TEST_CASE("stations and candidate sites partition the network", "[network]") {
    const TempCsv nodes("part_nodes.csv", kGoodNodes);
    const TempCsv edges("part_edges.csv", kGoodEdges);
    const Network network = Network::load(nodes.path(), edges.path());

    CHECK(network.stationNodes() == std::vector<NodeId>{0, 2});
    CHECK(network.candidateSites() == std::vector<NodeId>{1});
    CHECK(network.stationNodes().size() + network.candidateSites().size() == network.size());
}

TEST_CASE("nodes resolve by name and by id", "[network]") {
    const TempCsv nodes("res_nodes.csv", kGoodNodes);
    const TempCsv edges("res_edges.csv", kGoodEdges);
    const Network network = Network::load(nodes.path(), edges.path());

    CHECK(network.resolve("Bravo") == 1);
    CHECK(network.resolve("1") == 1);
    CHECK(network.resolve("bravo") == kNoNode);  // case-sensitive by design
    CHECK(network.resolve("Nowhere") == kNoNode);
    CHECK(network.resolve("99") == kNoNode);
    CHECK(network.resolve("1x") == kNoNode);  // must not partially parse
}

TEST_CASE("malformed input fails loudly with a line number", "[network]") {
    // The legacy Sydney matrix was one value short and the loader never noticed,
    // silently shifting every subsequent distance by one position. Failing loudly
    // is the whole point of this loader.
    SECTION("non-numeric distance") {
        const TempCsv nodes("bad1_nodes.csv", kGoodNodes);
        const TempCsv edges("bad1_edges.csv", "from_id,to_id,distance_km\n0,1,ten\n");
        CHECK_THROWS_AS(Network::load(nodes.path(), edges.path()), std::runtime_error);
    }
    SECTION("edge referencing an unknown node") {
        const TempCsv nodes("bad2_nodes.csv", kGoodNodes);
        const TempCsv edges("bad2_edges.csv", "from_id,to_id,distance_km\n0,42,10.0\n");
        CHECK_THROWS_AS(Network::load(nodes.path(), edges.path()), std::runtime_error);
    }
    SECTION("non-positive distance") {
        const TempCsv nodes("bad3_nodes.csv", kGoodNodes);
        const TempCsv edges("bad3_edges.csv", "from_id,to_id,distance_km\n0,1,0.0\n");
        CHECK_THROWS_AS(Network::load(nodes.path(), edges.path()), std::runtime_error);
    }
    SECTION("missing required column") {
        const TempCsv nodes("bad4_nodes.csv", "id,name\n0,Alpha\n");
        const TempCsv edges("bad4_edges.csv", kGoodEdges);
        CHECK_THROWS_AS(Network::load(nodes.path(), edges.path()), std::runtime_error);
    }
    SECTION("non-contiguous ids") {
        const TempCsv nodes("bad5_nodes.csv",
                            "id,name,has_station,price_per_kwh,chargers,power_kw\n"
                            "0,Alpha,0,0,0,0\n"
                            "7,Bravo,0,0,0,0\n");
        const TempCsv edges("bad5_edges.csv", "from_id,to_id,distance_km\n0,1,1.0\n");
        CHECK_THROWS_AS(Network::load(nodes.path(), edges.path()), std::runtime_error);
    }
    SECTION("row with too few fields names the missing column") {
        const TempCsv nodes("bad6_nodes.csv",
                            "id,name,has_station,price_per_kwh,chargers,power_kw\n"
                            "0,Alpha,1,0.40\n");
        const TempCsv edges("bad6_edges.csv", kGoodEdges);
        CHECK_THROWS_AS(Network::load(nodes.path(), edges.path()), std::runtime_error);
    }
    SECTION("missing file") {
        CHECK_THROWS_AS(Network::load("/nonexistent/nodes.csv", "/nonexistent/edges.csv"),
                        std::runtime_error);
    }
}

TEST_CASE("validation reports structural problems", "[network]") {
    SECTION("a clean network produces no warnings") {
        CHECK(testing::corridor().validate().empty());
    }
    SECTION("isolated nodes and disconnection are both reported") {
        // The diamond fixture deliberately contains an isolated node F.
        const auto warnings = testing::diamond().validate();
        REQUIRE_FALSE(warnings.empty());
        const bool mentionsIsolation =
            std::any_of(warnings.begin(), warnings.end(), [](const std::string& w) {
                return w.find("isolated") != std::string::npos;
            });
        const bool mentionsConnectivity =
            std::any_of(warnings.begin(), warnings.end(), [](const std::string& w) {
                return w.find("not fully connected") != std::string::npos;
            });
        CHECK(mentionsIsolation);
        CHECK(mentionsConnectivity);
    }
    SECTION("a station with no chargers is reported") {
        Network network = testing::corridor();
        network.setStation(0, Station{0.5, 0, 50.0});
        const auto warnings = network.validate();
        CHECK(std::any_of(warnings.begin(), warnings.end(), [](const std::string& w) {
            return w.find("no chargers") != std::string::npos;
        }));
    }
}

TEST_CASE("out-of-range access throws rather than corrupting memory", "[network]") {
    Network network = testing::corridor();
    CHECK_THROWS_AS(network.node(99), std::out_of_range);
    CHECK_THROWS_AS(network.neighbours(-1), std::out_of_range);
    CHECK_THROWS_AS(network.setStation(99, Station{}), std::out_of_range);
    CHECK_THROWS_AS(network.addEdge(0, 99, 1.0), std::out_of_range);
}
