#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstdio>
#include <fstream>
#include <numeric>

#include "evnet/simulation.hpp"
#include "fixtures.hpp"

using namespace evnet;
using Catch::Matchers::WithinAbs;

namespace {

Kwh totalCharged(const TripResult& result) {
    return std::accumulate(result.stops.begin(), result.stops.end(), 0.0,
                           [](Kwh sum, const Stop& stop) { return sum + stop.energyKwh; });
}

}  // namespace

TEST_CASE("a journey needing two stops completes", "[simulation]") {
    const Network network = testing::corridor();
    const Router router(network);
    const Allocator allocator(network, router);
    const CheapestEnergyPolicy policy;
    StationState state(network);

    const TripResult result = allocator.runOne(testing::corridorJourney(), policy, state);

    REQUIRE(result.completed);
    CHECK(result.failure.empty());
    CHECK(result.stops.size() == 2);
    CHECK(network.node(result.stops[0].node).name == "Mid1");
    CHECK(network.node(result.stops[1].node).name == "Mid2");
    CHECK_THAT(result.distanceKm, WithinAbs(300.0, 1e-9));
}

TEST_CASE("energy balances over a completed trip", "[simulation]") {
    const Network network = testing::corridor();
    const Router router(network);
    const Allocator allocator(network, router);
    const CheapestEnergyPolicy policy;
    StationState state(network);

    const Demand demand = testing::corridorJourney();
    const TripResult result = allocator.runOne(demand, policy, state);
    REQUIRE(result.completed);

    // Nothing may be created or destroyed: starting charge plus everything taken
    // on must cover the distance driven, and the surplus is the reserve the
    // allocator deliberately holds back.
    const Kwh supplied = demand.socKwh + totalCharged(result);
    const Kwh consumed = energyForDistance(result.distanceKm, demand.efficiency);
    CHECK(supplied >= consumed);

    const Kwh surplus = supplied - consumed;
    const Kwh reserve = demand.batteryKwh * allocator.config().reserveFraction;
    CHECK_THAT(surplus, WithinAbs(reserve, 1e-6));
}

TEST_CASE("no charging happens when the trip is already within range", "[simulation]") {
    const Network network = testing::corridor();
    const Router router(network);
    const Allocator allocator(network, router);
    const GeneralisedCostPolicy policy(20.0);
    StationState state(network);

    Demand demand = testing::corridorJourney();
    demand.destination = 1;  // Start -> Mid1 is only 100 km
    demand.socKwh = 25.0;    // 138 km of range

    const TripResult result = allocator.runOne(demand, policy, state);
    REQUIRE(result.completed);
    CHECK(result.stops.empty());
    CHECK_THAT(result.distanceKm, WithinAbs(100.0, 1e-9));
    CHECK(state.queueLength(1) == 0);
}

TEST_CASE("a vehicle with too little charge is reported stranded, not silently dropped",
          "[simulation]") {
    const Network network = testing::corridor();
    const Router router(network);
    const Allocator allocator(network, router);
    const CheapestEnergyPolicy policy;
    StationState state(network);

    Demand demand = testing::corridorJourney();
    demand.socKwh = 5.0;  // 27 km of range; the first station is 100 km away

    const TripResult result = allocator.runOne(demand, policy, state);
    CHECK_FALSE(result.completed);
    CHECK(result.failure.find("stranded at Start") != std::string::npos);
    CHECK(result.stops.empty());
}

TEST_CASE("the progress guard prevents backtracking to a cheaper station", "[simulation]") {
    // Start has a very cheap station, so a purely price-driven policy would want
    // to drive backwards to it from Mid1. Only stops that move the vehicle
    // strictly closer to its destination are admissible, which is what stops the
    // vehicle oscillating between two cheap stations forever.
    Network network = testing::corridor();
    network.setStation(0, Station{0.01, 8, 150.0});

    const Router router(network);
    const Allocator allocator(network, router);
    const CheapestEnergyPolicy policy;
    StationState state(network);

    Demand demand = testing::corridorJourney();
    demand.origin = 1;       // begin at Mid1
    demand.destination = 3;  // heading to Target
    demand.socKwh = 20.0;

    const TripResult result = allocator.runOne(demand, policy, state);
    REQUIRE(result.completed);
    for (const auto& stop : result.stops) {
        CHECK(network.node(stop.node).name != "Start");
    }
    CHECK(state.queueLength(0) == 0);
}

TEST_CASE("the onward-feasibility guard refuses a dead-end stop", "[simulation]") {
    // Mid2 is placed beyond the range a full charge at Mid1 would provide, and
    // there is nothing between them. A vehicle sent to Mid1 could never leave, so
    // Mid1 must not be offered at all and the trip is correctly reported as
    // infeasible rather than ending in a stranded vehicle mid-route.
    Network network;
    for (const char* name : {"Start", "Mid1", "Target"}) {
        Node node;
        node.name = name;
        network.addNode(node);
    }
    network.setStation(1, Station{0.30, 2, 100.0});
    network.addEdge(0, 1, 100.0);
    network.addEdge(1, 2, 400.0);  // unreachable on a 166 km full range

    const Router router(network);
    const Allocator allocator(network, router);
    const CheapestEnergyPolicy policy;
    StationState state(network);

    Demand demand = testing::corridorJourney();
    demand.destination = 2;

    const TripResult result = allocator.runOne(demand, policy, state);
    CHECK_FALSE(result.completed);
    CHECK(state.queueLength(1) == 0);
}

TEST_CASE("queues accumulate across a fleet so vehicles affect one another",
          "[simulation]") {
    const Network network = testing::corridor();
    const Router router(network);
    const Allocator allocator(network, router);
    const CheapestEnergyPolicy policy;
    StationState state(network);

    const std::vector<Demand> fleet(10, testing::corridorJourney());
    const auto results = allocator.run(fleet, policy, state);

    CHECK(results.size() == 10);
    for (const auto& result : results) CHECK(result.completed);

    // Every vehicle takes the same cheapest route, so both stations see all ten.
    CHECK(state.queueLength(1) == 10);
    CHECK(state.queueLength(2) == 10);

    // Wait scales with queue over chargers: Mid1 has 1 charger, Mid2 has 4.
    CHECK_THAT(state.expectedWait(1), WithinAbs(0.5 * 10 / 1.0, 1e-9));
    CHECK_THAT(state.expectedWait(2), WithinAbs(0.5 * 10 / 4.0, 1e-9));
}

TEST_CASE("a top-up mission is a round trip to a single station", "[simulation]") {
    const Network network = testing::corridor();
    const Router router(network);
    const Allocator allocator(network, router);
    const CheapestEnergyPolicy policy;
    StationState state(network);

    Demand demand;
    demand.id = 7;
    demand.origin = 0;
    demand.destination = 0;  // origin == destination marks a top-up
    demand.batteryKwh = 60.0;
    demand.socKwh = 40.0;  // 222 km of range
    demand.efficiency = 18.0;
    demand.requiredKwh = 15.0;

    REQUIRE(demand.isTopUp());
    const TripResult result = allocator.runOne(demand, policy, state);

    REQUIRE(result.completed);
    REQUIRE(result.stops.size() == 1);
    // Mid1 at $0.30 is cheaper than Mid2 at $0.60 and both are in range.
    CHECK(network.node(result.stops[0].node).name == "Mid1");
    CHECK_THAT(result.distanceKm, WithinAbs(200.0, 1e-9));  // 100 km out and back
    CHECK_THAT(result.stops[0].energyKwh, WithinAbs(15.0, 1e-6));
}

TEST_CASE("summaries report the tail, not just the mean", "[simulation]") {
    const Network network = testing::corridor();
    const Router router(network);
    const Allocator allocator(network, router);
    const CheapestEnergyPolicy policy;
    StationState state(network);

    const std::vector<Demand> fleet(20, testing::corridorJourney());
    const auto results = allocator.run(fleet, policy, state);
    const Summary summary = allocator.summarise(results, state, "cheapest");

    CHECK(summary.demands == 20);
    CHECK(summary.completed == 20);
    CHECK(summary.stranded == 0);
    CHECK_THAT(summary.meanStops, WithinAbs(2.0, 1e-9));
    CHECK(summary.peakQueue == 20);
    // Later vehicles queue behind earlier ones, so the tail must exceed the mean.
    CHECK(summary.p95WaitHours > summary.meanWaitHours);
}

TEST_CASE("demand loading rejects impossible vehicles", "[simulation]") {
    // Guards the class of silent-garbage parse the legacy corridor loader had.
    const std::string path = std::string(EVNET_PROJECT_ROOT) + "/tests/tmp_bad_demands.csv";
    {
        std::ofstream out(path);
        out << "id,origin_id,destination_id,battery_kwh,soc_kwh,efficiency_kwh_per_100km,required_kwh\n";
        out << "1,0,3,30.0,45.0,18.0,0.0\n";  // more charge than the battery holds
    }
    CHECK_THROWS_AS(Demand::load(path), std::runtime_error);
    std::remove(path.c_str());
}

TEST_CASE("a vehicle at a station can charge without moving", "[simulation]") {
    // Regression: reachableWithin deliberately excludes the origin, so the
    // vehicle's own node was never offered as a charging option. A car parked at
    // a station with free chargers was therefore declared stranded. Two vehicles
    // in the shipped Hume fleet hit exactly this, both sitting at Yass.
    Network network = testing::corridor();
    network.setStation(0, Station{0.40, 4, 100.0});  // a charger at the origin

    const Router router(network);
    const Allocator allocator(network, router);
    const CheapestEnergyPolicy policy;
    StationState state(network);

    Demand demand = testing::corridorJourney();
    demand.destination = 1;  // 100 km away
    demand.socKwh = 10.0;    // only 55 km of range: cannot reach, must charge here

    const TripResult result = allocator.runOne(demand, policy, state);

    REQUIRE(result.completed);
    REQUIRE(result.stops.size() == 1);
    CHECK(result.stops[0].node == 0);                 // charged in place
    CHECK_THAT(result.distanceKm, WithinAbs(100.0, 1e-9));  // no detour incurred
    CHECK(state.queueLength(0) == 1);
}

TEST_CASE("charging in place cannot loop forever", "[simulation]") {
    // Charging in place is exempt from the progress guard, so it needs its own
    // termination argument: a second attempt at the same node yields no useful
    // energy and is filtered out, leaving the vehicle stranded rather than
    // spinning until the stop limit.
    Network network = testing::corridor();
    network.setStation(0, Station{0.40, 4, 100.0});

    const Router router(network);
    const Allocator allocator(network, router);
    const CheapestEnergyPolicy policy;
    StationState state(network);

    Demand demand = testing::corridorJourney();
    demand.destination = 3;
    demand.batteryKwh = 12.0;  // 66 km full range; every 100 km leg is impossible
    demand.socKwh = 2.0;

    const TripResult result = allocator.runOne(demand, policy, state);
    CHECK_FALSE(result.completed);
    CHECK(result.failure.find("stranded") != std::string::npos);
    // One top-up in place at most, never the full stop budget burned on cycling.
    CHECK(state.queueLength(0) <= 1);
}

TEST_CASE("greedy price minimisation trades many stops for small savings",
          "[simulation]") {
    // A documented property rather than a defect: every policy here is greedy on
    // the next stop only. On the real Hume corridor this makes `cheapest` take
    // eight short hops to save about two dollars of energy against `farthest`'s
    // two long ones. Global optimisation over (node, charge) state is stage 2
    // work; this test pins the current behaviour so that change is visible.
    const Network network = testing::corridor();
    const Router router(network);
    const Allocator allocator(network, router);

    Demand demand = testing::corridorJourney();
    demand.batteryKwh = 40.0;
    demand.socKwh = 20.0;

    StationState cheapState(network);
    const auto cheap = allocator.runOne(demand, CheapestEnergyPolicy(), cheapState);
    StationState farState(network);
    const auto far = allocator.runOne(demand, FarthestReachablePolicy(), farState);

    REQUIRE(cheap.completed);
    REQUIRE(far.completed);
    // Same road distance either way; only the stop pattern and price differ.
    CHECK_THAT(cheap.distanceKm, WithinAbs(far.distanceKm, 1e-9));
    CHECK(cheap.energyCost <= far.energyCost);
    CHECK(cheap.stops.size() >= far.stops.size());
}
