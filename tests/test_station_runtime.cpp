#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "evnet/station_runtime.hpp"
#include "fixtures.hpp"

using namespace evnet;
using Catch::Matchers::WithinAbs;

namespace {

/// One station, `chargers` chargers, 100 kW, no session overhead so the arithmetic
/// in these tests stays legible.
Network singleStation(int chargers) {
    Network network;
    Node node;
    node.name = "Hub";
    node.station = Station{0.50, chargers, 100.0};
    network.addNode(node);
    return network;
}

}  // namespace

TEST_CASE("an idle station imposes no wait", "[runtime]") {
    const Network network = singleStation(2);
    const StationRuntime runtime(network);
    CHECK_THAT(runtime.expectedWait(0, 0.0), WithinAbs(0.0, 1e-12));
    CHECK_THAT(runtime.expectedWait(0, 5.0), WithinAbs(0.0, 1e-12));
    CHECK(runtime.records(0).empty());
}

TEST_CASE("chargers are used in parallel before anyone queues", "[runtime]") {
    const Network network = singleStation(2);
    StationRuntime runtime(network);

    // Two vehicles, two chargers: neither waits.
    const auto first = runtime.admit(0, 1, 0.0, 100.0);   // 1.0h of charging
    const auto second = runtime.admit(0, 2, 0.0, 100.0);
    CHECK_THAT(first.wait(), WithinAbs(0.0, 1e-12));
    CHECK_THAT(second.wait(), WithinAbs(0.0, 1e-12));
    CHECK_THAT(first.service(), WithinAbs(1.0, 1e-12));

    // The third must wait for the earliest to free up at t=1.0.
    const auto third = runtime.admit(0, 3, 0.0, 50.0);
    CHECK_THAT(third.start, WithinAbs(1.0, 1e-12));
    CHECK_THAT(third.wait(), WithinAbs(1.0, 1e-12));
    CHECK_THAT(third.finish, WithinAbs(1.5, 1e-12));
}

TEST_CASE("waits are measured, and vanish for a late enough arrival", "[runtime]") {
    // This is the property stage 1 could not express at all: its queue only ever
    // grew, so a vehicle turning up after the rush was charged for congestion that
    // had long since cleared.
    const Network network = singleStation(1);
    StationRuntime runtime(network);

    runtime.admit(0, 1, 0.0, 100.0);  // occupies the single charger until t=1.0
    CHECK_THAT(runtime.expectedWait(0, 0.0), WithinAbs(1.0, 1e-12));
    CHECK_THAT(runtime.expectedWait(0, 0.5), WithinAbs(0.5, 1e-12));
    CHECK_THAT(runtime.expectedWait(0, 1.0), WithinAbs(0.0, 1e-12));
    CHECK_THAT(runtime.expectedWait(0, 9.0), WithinAbs(0.0, 1e-12));

    const auto later = runtime.admit(0, 2, 4.0, 100.0);
    CHECK_THAT(later.wait(), WithinAbs(0.0, 1e-12));
}

TEST_CASE("earliest-free assignment reproduces station-wide FIFO", "[runtime]") {
    // The simulator feeds arrivals in nondecreasing time order, and each takes the
    // charger that frees soonest. That is claimed to be equivalent to one FIFO queue
    // across the station; this checks the consequence, that start times are
    // nondecreasing in arrival order and nobody overtakes.
    const Network network = singleStation(3);
    StationRuntime runtime(network);

    std::vector<ServiceRecord> served;
    for (int i = 0; i < 12; ++i) {
        served.push_back(runtime.admit(0, i, static_cast<Hours>(i) * 0.1, 100.0));
    }
    for (std::size_t i = 1; i < served.size(); ++i) {
        CHECK(served[i].start >= served[i - 1].start);
        CHECK(served[i].arrival >= served[i - 1].arrival);
    }
    // Three chargers, so the first three go straight on.
    for (int i = 0; i < 3; ++i) CHECK_THAT(served[static_cast<std::size_t>(i)].wait(), WithinAbs(0.0, 1e-12));
    CHECK(served[3].wait() > 0.0);
}

TEST_CASE("occupancy queries agree with the records they derive from", "[runtime]") {
    const Network network = singleStation(1);
    StationRuntime runtime(network);
    runtime.admit(0, 1, 0.0, 100.0);  // charging over [0, 1)
    runtime.admit(0, 2, 0.0, 100.0);  // waiting over [0, 1), charging over [1, 2)

    CHECK(runtime.chargingAt(0, 0.5) == 1);
    CHECK(runtime.waitingAt(0, 0.5) == 1);
    CHECK(runtime.chargingAt(0, 1.5) == 1);
    CHECK(runtime.waitingAt(0, 1.5) == 0);
    CHECK(runtime.chargingAt(0, 2.5) == 0);

    const auto [peak, when] = runtime.peakWaiting(0);
    CHECK(peak == 1);
    CHECK(when >= 0.0);
}

TEST_CASE("utilisation stays within bounds and clips to the horizon", "[runtime]") {
    const Network network = singleStation(2);
    StationRuntime runtime(network);
    runtime.admit(0, 1, 0.0, 100.0);  // one charger busy for 1h of a 2-charger station

    CHECK_THAT(runtime.utilisation(0, 2.0), WithinAbs(0.25, 1e-9));  // 1 of 4 charger-hours
    CHECK_THAT(runtime.utilisation(0, 1.0), WithinAbs(0.5, 1e-9));
    // A session extending past the window must not push utilisation above 1.
    CHECK(runtime.utilisation(0, 0.25) <= 1.0);
    CHECK_THAT(runtime.utilisation(0, 0.0), WithinAbs(0.0, 1e-12));
}

TEST_CASE("nodes without chargers are inert rather than dangerous", "[runtime]") {
    const Network network = testing::corridor();  // node 0 "Start" has no station
    StationRuntime runtime(network);
    CHECK_THAT(runtime.expectedWait(0, 0.0), WithinAbs(0.0, 1e-12));
    CHECK_THAT(runtime.chargeTime(0, 50.0), WithinAbs(0.0, 1e-12));
    CHECK_THROWS_AS(runtime.admit(0, 1, 0.0, 10.0), std::runtime_error);
}

TEST_CASE("reset returns the runtime to a clean slate", "[runtime]") {
    const Network network = singleStation(1);
    StationRuntime runtime(network);
    runtime.admit(0, 1, 0.0, 100.0);
    REQUIRE(runtime.records(0).size() == 1);
    REQUIRE(runtime.lastFinish() > 0.0);

    runtime.reset();
    CHECK(runtime.records(0).empty());
    CHECK_THAT(runtime.lastFinish(), WithinAbs(0.0, 1e-12));
    CHECK_THAT(runtime.expectedWait(0, 0.0), WithinAbs(0.0, 1e-12));
}

TEST_CASE("allRecords is sorted and complete", "[runtime]") {
    const Network network = testing::corridor();
    StationRuntime runtime(network);
    runtime.admit(2, 1, 3.0, 10.0);
    runtime.admit(1, 2, 1.0, 10.0);
    runtime.admit(1, 3, 2.0, 10.0);

    const auto all = runtime.allRecords();
    REQUIRE(all.size() == 3);
    for (std::size_t i = 1; i < all.size(); ++i) {
        CHECK(all[i].start >= all[i - 1].start);
    }
}
