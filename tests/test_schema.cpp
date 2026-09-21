#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

#include "evnet/network.hpp"
#include "evnet/simulation.hpp"

// A dataset may name its columns in its own domain's language -- `chargers` and
// `battery_kwh` for an EV study, as every shipped dataset does -- or in the
// engine's neutral one: `servers`, `capacity`, `level`. A truck rest area has
// bays, not chargers, and a driver's clock is not a battery.

using namespace evnet;
using Catch::Matchers::ContainsSubstring;

namespace {

class TempFile {
public:
    TempFile(const std::string& name, const std::string& contents)
        : path_(std::string(EVNET_PROJECT_ROOT) + "/tests/tmp_schema_" + name) {
        std::ofstream(path_) << contents;
    }
    ~TempFile() { std::remove(path_.c_str()); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

const char* kEdges = "from_id,to_id,distance_km\n0,1,100\n";

std::string loadError(const std::string& nodes) {
    const TempFile n("nodes.csv", nodes);
    const TempFile e("edges.csv", kEdges);
    try {
        Network::load(n.path(), e.path());
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    return "";
}

}  // namespace

TEST_CASE("a network may name its station columns neutrally", "[schema]") {
    const TempFile ev("ev_nodes.csv",
                      "id,name,has_station,price_per_kwh,chargers,power_kw\n0,A,1,0.45,3,50\n1,B,0,0,0,0\n");
    const TempFile neutral("neutral_nodes.csv",
                           "id,name,has_station,price_per_unit,servers,rate_per_hour\n0,A,1,0.45,3,50\n1,B,0,0,0,0\n");
    const TempFile edges("edges.csv", kEdges);

    const Network a = Network::load(ev.path(), edges.path());
    const Network b = Network::load(neutral.path(), edges.path());
    REQUIRE(b.node(0).hasStation());
    CHECK(b.node(0).station->pricePerUnit == a.node(0).station->pricePerUnit);
    CHECK(b.node(0).station->servers == a.node(0).station->servers);
    CHECK(b.node(0).station->ratePerHour == a.node(0).station->ratePerHour);
}

TEST_CASE("a column named both ways, or neither, is an error that says so", "[schema]") {
    SECTION("both") {
        const std::string error =
            loadError("id,name,has_station,price_per_kwh,chargers,servers,power_kw\n0,A,1,0.45,3,3,50\n1,B,0,0,0,0,0\n");
        CHECK_THAT(error, ContainsSubstring("'chargers'") && ContainsSubstring("'servers'") &&
                              ContainsSubstring("more than one"));
    }
    SECTION("neither") {
        const std::string error = loadError("id,name,has_station,price_per_kwh,power_kw\n0,A,1,0.45,50\n1,B,0,0,0\n");
        CHECK_THAT(error, ContainsSubstring("'chargers'") && ContainsSubstring("'servers'"));
    }
}

TEST_CASE("a demand file may name its columns neutrally", "[schema]") {
    const TempFile ev("ev_demands.csv",
                      "id,origin_id,destination_id,battery_kwh,soc_kwh,efficiency_kwh_per_100km,required_kwh,release_hour\n"
                      "7,0,1,60,30,18,0,2.5\n");
    const TempFile neutral("neutral_demands.csv",
                           "id,origin_id,destination_id,capacity,level,consumption,required_amount,release_hour\n"
                           "7,0,1,60,30,18,0,2.5\n");
    const auto a = Demand::load(ev.path());
    const auto b = Demand::load(neutral.path());
    REQUIRE(a.size() == 1);
    REQUIRE(b.size() == 1);
    CHECK(b[0].capacity == a[0].capacity);
    CHECK(b[0].level == a[0].level);
    CHECK(b[0].consumption == a[0].consumption);
    CHECK(b[0].requiredAmount == a[0].requiredAmount);
    CHECK(b[0].releaseHour == a[0].releaseHour);
}
