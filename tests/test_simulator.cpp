#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <numeric>

#include "evnet/simulator.hpp"
#include "fixtures.hpp"

using namespace evnet;
using Catch::Matchers::WithinAbs;

namespace {

SimulatorConfig fastConfig() {
    SimulatorConfig config;
    config.speedKmh = 100.0;      // 100 km per hour keeps the arithmetic legible
    config.stopOverheadHours = 0.0;  // isolate queueing from session overhead
    return config;
}

std::unique_ptr<Planner> greedy(const Network& network, const Router& router,
                                const std::string& name, const SimulatorConfig& config) {
    return makePlanner(name, network, router, config.valueOfTimePerHour, config.feasibility());
}

}  // namespace

TEST_CASE("driving consumes wall-clock time", "[simulator]") {
    // Stage 1 had no clock at all, so a 300 km journey and a 3 km one finished at the
    // same instant.
    const Network network = testing::corridor();
    const Router router(network);
    const Simulator simulator(network, router, fastConfig());
    auto planner = greedy(network, router, "cheapest", fastConfig());

    StationRuntime runtime(network, 0.0);
    const auto trips = simulator.run({testing::corridorJourney()}, *planner, runtime);

    REQUIRE(trips.size() == 1);
    REQUIRE(trips[0].completed);
    // 300 km at 100 km/h is 3 hours on the road, plus whatever charging took.
    CHECK_THAT(trips[0].drivingHours, WithinAbs(3.0, 1e-9));
    CHECK(trips[0].elapsed() > trips[0].drivingHours);
    CHECK(trips[0].finishTime > 0.0);
}

TEST_CASE("release times are honoured", "[simulator]") {
    const Network network = testing::corridor();
    const Router router(network);
    const Simulator simulator(network, router, fastConfig());
    auto planner = greedy(network, router, "cheapest", fastConfig());

    Demand late = testing::corridorJourney();
    late.releaseHour = 6.0;

    StationRuntime runtime(network, 0.0);
    const auto trips = simulator.run({late}, *planner, runtime);

    REQUIRE(trips[0].completed);
    CHECK_THAT(trips[0].releaseTime, WithinAbs(6.0, 1e-9));
    CHECK(trips[0].finishTime >= 6.0);
    // Nothing may be served before its vehicle has even set off.
    for (const auto& record : runtime.allRecords()) CHECK(record.arrival >= 6.0);
}

TEST_CASE("a lone vehicle never queues", "[simulator]") {
    const Network network = testing::corridor();
    const Router router(network);
    const Simulator simulator(network, router, fastConfig());
    auto planner = greedy(network, router, "cheapest", fastConfig());

    StationRuntime runtime(network, 0.0);
    const auto trips = simulator.run({testing::corridorJourney()}, *planner, runtime);
    CHECK_THAT(trips[0].waitHours, WithinAbs(0.0, 1e-9));
    CHECK(trips[0].chargeHours > 0.0);
}

TEST_CASE("simultaneous departures queue, staggered ones do not", "[simulator]") {
    // The central improvement over stage 1, where both cases produced identical
    // (and steadily growing) congestion.
    Network network = testing::corridor();
    network.setStation(1, Station{0.30, 1, 50.0});  // one slow charger: a bottleneck

    const Router router(network);
    const Simulator simulator(network, router, fastConfig());
    auto planner = greedy(network, router, "cheapest", fastConfig());

    std::vector<Demand> together(6, testing::corridorJourney());
    for (std::size_t i = 0; i < together.size(); ++i) together[i].id = static_cast<int>(i);

    std::vector<Demand> spread = together;
    for (std::size_t i = 0; i < spread.size(); ++i) spread[i].releaseHour = static_cast<Hours>(i) * 8.0;

    StationRuntime crowdedRuntime(network, 0.0);
    const auto crowded = simulator.run(together, *planner, crowdedRuntime);
    StationRuntime spreadRuntime(network, 0.0);
    const auto relaxed = simulator.run(spread, *planner, spreadRuntime);

    const auto meanWait = [](const std::vector<TimedTrip>& trips) {
        return std::accumulate(trips.begin(), trips.end(), 0.0,
                               [](Hours sum, const TimedTrip& t) { return sum + t.waitHours; }) /
               static_cast<double>(trips.size());
    };

    CHECK(meanWait(crowded) > 0.0);
    CHECK_THAT(meanWait(relaxed), WithinAbs(0.0, 1e-9));
    CHECK(crowdedRuntime.peakWaiting(1).first > spreadRuntime.peakWaiting(1).first);
}

TEST_CASE("energy balances across an event-driven trip", "[simulator]") {
    const Network network = testing::corridor();
    const Router router(network);
    const Simulator simulator(network, router, fastConfig());
    auto planner = greedy(network, router, "cheapest", fastConfig());

    const Demand demand = testing::corridorJourney();
    StationRuntime runtime(network, 0.0);
    const auto trips = simulator.run({demand}, *planner, runtime);
    REQUIRE(trips[0].completed);

    const Kwh charged = std::accumulate(
        trips[0].stops.begin(), trips[0].stops.end(), 0.0,
        [](Kwh sum, const Stop& s) { return sum + s.energyKwh; });
    const Kwh consumed = energyForDistance(trips[0].distanceKm, demand.efficiency);
    CHECK(demand.socKwh + charged + 1e-6 >= consumed);
}

TEST_CASE("every charging session in a trip has a matching service record", "[simulator]") {
    const Network network = testing::corridor();
    const Router router(network);
    const Simulator simulator(network, router, fastConfig());
    auto planner = greedy(network, router, "min-wait", fastConfig());

    std::vector<Demand> fleet(15, testing::corridorJourney());
    for (std::size_t i = 0; i < fleet.size(); ++i) fleet[i].id = static_cast<int>(i);

    StationRuntime runtime(network, 0.0);
    const auto trips = simulator.run(fleet, *planner, runtime);

    std::size_t stops = 0;
    for (const auto& trip : trips) stops += trip.stops.size();
    CHECK(stops == runtime.allRecords().size());
}

TEST_CASE("the simulation is deterministic", "[simulator]") {
    // The output is a comparison between planners, so a run that varies would make
    // every reported difference meaningless.
    const Network network = testing::corridor();
    const Router router(network);
    const Simulator simulator(network, router, fastConfig());

    std::vector<Demand> fleet(20, testing::corridorJourney());
    for (std::size_t i = 0; i < fleet.size(); ++i) {
        fleet[i].id = static_cast<int>(i);
        fleet[i].releaseHour = static_cast<Hours>(i % 5) * 0.5;
    }

    auto first = greedy(network, router, "generalised", fastConfig());
    StationRuntime runtimeA(network, 0.0);
    const auto a = simulator.run(fleet, *first, runtimeA);

    auto second = greedy(network, router, "generalised", fastConfig());
    StationRuntime runtimeB(network, 0.0);
    const auto b = simulator.run(fleet, *second, runtimeB);

    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].completed == b[i].completed);
        CHECK_THAT(a[i].finishTime, WithinAbs(b[i].finishTime, 1e-12));
        CHECK_THAT(a[i].waitHours, WithinAbs(b[i].waitHours, 1e-12));
        CHECK(a[i].stops.size() == b[i].stops.size());
    }
}

TEST_CASE("a vehicle that cannot move is reported, not lost", "[simulator]") {
    const Network network = testing::corridor();
    const Router router(network);
    const Simulator simulator(network, router, fastConfig());
    auto planner = greedy(network, router, "cheapest", fastConfig());

    Demand stuck = testing::corridorJourney();
    stuck.socKwh = 5.0;  // 27 km of range; the nearest station is 100 km off

    StationRuntime runtime(network, 0.0);
    const auto trips = simulator.run({stuck}, *planner, runtime);
    CHECK_FALSE(trips[0].completed);
    CHECK(trips[0].failure.find("stranded") != std::string::npos);
    CHECK(runtime.allRecords().empty());
}

TEST_CASE("top-up missions run through the event engine too", "[simulator]") {
    const Network network = testing::corridor();
    const Router router(network);
    const Simulator simulator(network, router, fastConfig());
    auto planner = greedy(network, router, "cheapest", fastConfig());

    Demand topUp;
    topUp.id = 5;
    topUp.origin = 0;
    topUp.destination = 0;
    topUp.batteryKwh = 60.0;
    topUp.socKwh = 40.0;
    topUp.efficiency = 18.0;
    topUp.requiredKwh = 15.0;

    StationRuntime runtime(network, 0.0);
    const auto trips = simulator.run({topUp}, *planner, runtime);
    REQUIRE(trips[0].completed);
    REQUIRE(trips[0].stops.size() == 1);
    CHECK(network.node(trips[0].stops[0].node).name == "Mid1");
    CHECK_THAT(trips[0].distanceKm, WithinAbs(200.0, 1e-9));  // out and back
    CHECK_THAT(trips[0].drivingHours, WithinAbs(2.0, 1e-9));  // 200 km at 100 km/h
}

TEST_CASE("the optimal planner beats greedy on generalised cost when uncongested",
          "[simulator][planner]") {
    // With an empty network there is no forecasting error to trip over, so lookahead
    // should be a straight win. The corridor gives it something to work with: Mid1 is
    // cheap, Mid2 is dear, and a greedy price-chaser stops more often than it needs to.
    const Network network = testing::corridor();
    const Router router(network);
    SimulatorConfig config = fastConfig();
    config.stopOverheadHours = 0.25;  // stops cost something, as they do in reality

    const Simulator simulator(network, router, config);
    Demand demand = testing::corridorJourney();
    demand.batteryKwh = 40.0;
    demand.socKwh = 20.0;

    Dollars bestGreedy = std::numeric_limits<Dollars>::infinity();
    for (const auto& name : {"farthest", "cheapest", "min-wait", "generalised"}) {
        auto planner = makePlanner(name, network, router, config.valueOfTimePerHour,
                                   config.feasibility());
        StationRuntime runtime(network, config.stopOverheadHours);
        const auto trips = simulator.run({demand}, *planner, runtime);
        REQUIRE(trips[0].completed);
        bestGreedy = std::min(bestGreedy, trips[0].generalisedCost(config.valueOfTimePerHour));
    }

    auto optimal = makePlanner("optimal", network, router, config.valueOfTimePerHour,
                               config.feasibility());
    StationRuntime runtime(network, config.stopOverheadHours);
    const auto trips = simulator.run({demand}, *optimal, runtime);
    REQUIRE(trips[0].completed);
    CHECK(trips[0].generalisedCost(config.valueOfTimePerHour) <= bestGreedy + 1e-6);
}

TEST_CASE("session overhead makes fewer, larger stops preferable", "[planner]") {
    // Without a per-stop cost the model is indifferent to fragmentation, because
    // total energy -- and so total transfer time -- is fixed by the distance. The
    // optimal planner exploited that precisely as it should have, splitting one
    // charge into six. This pins the fix.
    const Network network = testing::corridor();
    const Router router(network);

    Demand demand = testing::corridorJourney();
    demand.batteryKwh = 40.0;
    demand.socKwh = 20.0;

    const auto stopsWithOverhead = [&](Hours overhead) {
        SimulatorConfig config = fastConfig();
        config.stopOverheadHours = overhead;
        const Simulator simulator(network, router, config);
        auto planner = makePlanner("optimal", network, router, config.valueOfTimePerHour,
                                   config.feasibility());
        StationRuntime runtime(network, overhead);
        const auto trips = simulator.run({demand}, *planner, runtime);
        REQUIRE(trips[0].completed);
        return trips[0].stops.size();
    };

    CHECK(stopsWithOverhead(0.5) <= stopsWithOverhead(0.0));
}

TEST_CASE("planner factory covers every advertised planner", "[planner]") {
    const Network network = testing::corridor();
    const Router router(network);
    const auto names = plannerNames();
    CHECK(names.size() == 5);
    for (const auto& name : names) {
        const auto planner = makePlanner(name, network, router, 20.0);
        REQUIRE(planner != nullptr);
        CHECK(planner->name() == name);
    }
    CHECK_THROWS_AS(makePlanner("nonsense", network, router, 20.0), std::invalid_argument);
}

TEST_CASE("planners agree that a vehicle with plenty of charge should just drive",
          "[planner]") {
    const Network network = testing::corridor();
    const Router router(network);
    const StationRuntime runtime(network, 0.0);

    VehicleState vehicle;
    vehicle.at = 0;
    vehicle.destination = 1;  // 100 km
    vehicle.batteryKwh = 60.0;
    vehicle.socKwh = 50.0;    // 277 km of range
    vehicle.efficiency = 18.0;

    for (const auto& name : plannerNames()) {
        const auto planner = makePlanner(name, network, router, 20.0);
        const Action action = planner->decide(vehicle, runtime);
        CHECK(action.kind == Action::Kind::DriveToDestination);
    }
}

TEST_CASE("the time series samples every station across the window", "[simulator]") {
    const Network network = testing::corridor();
    const Router router(network);
    const Simulator simulator(network, router, fastConfig());
    auto planner = greedy(network, router, "cheapest", fastConfig());

    std::vector<Demand> fleet(8, testing::corridorJourney());
    for (std::size_t i = 0; i < fleet.size(); ++i) fleet[i].id = static_cast<int>(i);

    StationRuntime runtime(network, 0.0);
    const auto trips = simulator.run(fleet, *planner, runtime);
    const TimedSummary summary = simulator.summarise(trips, runtime, "cheapest");

    const auto samples = sampleTimeSeries(network, runtime, summary.makespan, 0.5);
    const auto stations = network.stationNodes().size();
    REQUIRE_FALSE(samples.empty());
    CHECK(samples.size() % stations == 0);
    for (const auto& sample : samples) {
        CHECK(sample.charging <= sample.chargers);
        CHECK(sample.waiting >= 0);
        CHECK(sample.time <= summary.makespan + 1e-9);
    }
    // Degenerate arguments must not produce garbage.
    CHECK(sampleTimeSeries(network, runtime, 1.0, 0.0).empty());
}

TEST_CASE("the optimal planner may arrive at a charger below its reserve", "[planner]") {
    // The reserve exists to stop a vehicle being stranded BETWEEN chargers, so it must
    // not be demanded on arrival AT one -- rolling in nearly empty is what the charger
    // is for. Enforcing it everywhere made this planner reject journeys the greedy
    // planners completed, which meant the two were being compared on different
    // feasibility problems rather than on the quality of their decisions.
    const Network network = testing::corridor();
    const Router router(network);
    const SimulatorConfig config = fastConfig();
    const Simulator simulator(network, router, config);

    Demand demand = testing::corridorJourney();
    demand.batteryKwh = 40.0;  // 222 km on a full charge, 200 km once the reserve is held
    demand.socKwh = 20.0;      // 111 km: reaches Mid1 with ~2 kWh, under the 4 kWh
                               // reserve but standing at a charger

    auto optimal = makePlanner("optimal", network, router, config.valueOfTimePerHour,
                               config.feasibility());
    StationRuntime runtime(network);
    const auto trips = simulator.run({demand}, *optimal, runtime);

    CHECK(trips[0].completed);
    CHECK_FALSE(trips[0].stops.empty());
}

TEST_CASE("but it may not drive away from one still below its reserve", "[planner]") {
    // The other half of the rule above. Arriving at a charger under the reserve is
    // fine; leaving one still under it is not, because the reserve's whole job is to
    // cover the leg AFTER the stop. Without this the waiver was collectable without
    // being paid for: a plan could coast into an expensive charger, decline to plug in,
    // and coast out again on fumes to reach a cheaper one.
    //
    // The cost of the leak was a free lunch rather than a wrong answer -- on a
    // 62-station network the old rule understated the fleet's mean generalised cost by
    // about $0.05 a trip -- but it flattered this planner in precisely the comparison
    // the toolkit exists to make, and it did so by spending a safety margin.
    //
    // The layout makes the temptation explicit: Near is dear, Far is cheap, and a
    // vehicle can just about coast from Near to Far on less than its reserve.
    Network network;
    Node start;
    start.name = "Start";
    network.addNode(start);
    Node near;
    near.name = "Near";
    near.station = Station{0.90, 4, 100.0};  // dear: the planner would rather skip it
    network.addNode(near);
    Node far;
    far.name = "Far";
    far.station = Station{0.20, 4, 100.0};  // cheap: worth coasting to, if that were legal
    network.addNode(far);
    Node target;
    target.name = "Target";
    network.addNode(target);
    network.addEdge(0, 1, 60.0);
    network.addEdge(1, 2, 10.0);
    network.addEdge(2, 3, 10.0);

    const Router router(network);
    const SimulatorConfig config = fastConfig();
    const Simulator simulator(network, router, config);

    Demand demand;
    demand.id = 1;
    demand.origin = 0;
    demand.destination = 3;
    demand.batteryKwh = 40.0;  // reserve is 4 kWh
    demand.socKwh = 13.0;      // 72 km of range: reaches Near with 2.2 kWh, under reserve
    demand.efficiency = 18.0;

    auto optimal = makePlanner("optimal", network, router, config.valueOfTimePerHour,
                               config.feasibility());
    StationRuntime runtime(network);
    const auto trips = simulator.run({demand}, *optimal, runtime);

    REQUIRE(trips[0].completed);
    const NodeId nearId = network.findByName("Near");
    const bool chargedAtNear =
        std::any_of(trips[0].stops.begin(), trips[0].stops.end(),
                    [nearId](const Stop& s) { return s.node == nearId; });
    CHECK(chargedAtNear);
}
