#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "domain_laws.hpp"
#include "evnet/domain.hpp"
#include "evnet/planner.hpp"
#include "evnet/simulation.hpp"
#include "evnet/simulator.hpp"
#include "evnet/station_runtime.hpp"

// The second domain: a solo heavy-vehicle driver under the Heavy Vehicle
// National Law's standard hours. 12 hours' work, restored in full by a 7-hour
// rest, at rest areas that turn trucks away when every bay is taken.

using namespace evnet;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

/// Every driver's work clock runs for all of its driving time.
constexpr PerDistance kAllDrivingIsWork = 1.0;

std::shared_ptr<const HeavyVehicleStandardHours> truck(double speedKmh = SimulatorConfig{}.speedKmh) {
    return std::make_shared<HeavyVehicleStandardHours>(speedKmh);
}

/// A four-stop corridor, every stop a rest area with `bays` truck bays.
///
///   Depot --300-- R1 --250-- R2 --250-- R3 --300-- Port        (1100 km)
Network restAreaCorridor(int bays) {
    Network network;
    const auto place = [&](const char* name, bool restArea) {
        Node node;
        node.name = name;
        if (restArea) node.station = Station{0.0, bays, 0.0};  // free; rate meaningless
        network.addNode(node);
    };
    place("Depot", false);
    place("R1", true);
    place("R2", true);
    place("R3", true);
    place("Port", false);
    network.addEdge(0, 1, 300.0);
    network.addEdge(1, 2, 250.0);
    network.addEdge(2, 3, 250.0);
    network.addEdge(3, 4, 300.0);
    network.setDomain(truck());
    return network;
}

Demand haul(int id, Hours alreadyWorked, Hours release = 0.0) {
    Demand d;
    d.id = id;
    d.origin = 0;
    d.destination = 4;
    d.capacity = HeavyVehicleStandardHours::kMaxWorkHours;
    d.level = HeavyVehicleStandardHours::kMaxWorkHours - alreadyWorked;
    d.consumption = kAllDrivingIsWork;
    d.releaseHour = release;
    return d;
}

std::vector<TimedTrip> simulate(const Network& network, const std::vector<Demand>& demands,
                                const std::string& planner) {
    const Router router(network);
    const SimulatorConfig config;
    const auto p = makePlanner(planner, network, router, config.valueOfTimePerHour, config.feasibility());
    StationRuntime runtime(network, config.stopOverheadHours);
    return Simulator(network, router, config).run(demands, *p, runtime);
}

}  // namespace

TEST_CASE("the truck domain obeys every domain law", "[truck][domain]") {
    for (const double speed : {60.0, 80.0, 100.0}) {
        for (const PerDistance fraction : {1.0, 0.5}) {
            testing::checkDomainLaws(HeavyVehicleStandardHours(speed), fraction, 0.0);
        }
    }
}

TEST_CASE("the truck domain encodes standard hours", "[truck][domain]") {
    const HeavyVehicleStandardHours d(80.0);

    SECTION("the clock runs at the speed of the truck") {
        CHECK_THAT(d.resourceForDistance(400.0, kAllDrivingIsWork), WithinAbs(5.0, 1e-12));
        CHECK_THAT(d.distanceOnResource(12.0, kAllDrivingIsWork), WithinAbs(960.0, 1e-9));
    }
    SECTION("a rest takes seven hours, whatever it restores") {
        CHECK(d.serviceDuration(0.5, 0.0) == 7.0);
        CHECK(d.serviceDuration(11.5, 123.0) == 7.0);
        CHECK(d.serviceDuration(0.0, 0.0) == 0.0);
    }
    SECTION("rest at all and all twelve hours come back") {
        CHECK(d.levelAfterService(3.0, 3.5, 12.0) == 12.0);
        CHECK(d.levelAfterService(0.0, 12.0, 12.0) == 12.0);
        CHECK(d.levelAfterService(5.0, 5.0, 12.0) == 5.0);  // asked for nothing
    }
    SECTION("full rest areas turn trucks away") { CHECK(d.admission() == Admission::TurnAway); }
    SECTION("a domain without a speed is an error, not a zero") {
        CHECK_THROWS_AS(HeavyVehicleStandardHours(0.0), std::invalid_argument);
    }
}

TEST_CASE("a driver's clock runs for exactly the time the simulator spends driving", "[truck]") {
    // The domain and the simulator each know a speed. If they disagreed, the
    // legal clock would drift away from the time actually driven -- so the
    // domain is built from the simulator's speed, and this checks they agree.
    for (const std::string& planner : plannerNames()) {
        INFO("planner " << planner);
        const Network network = restAreaCorridor(4);
        // 1100 km at 80 km/h is 13.75h: a fresh driver must rest once.
        const auto trips = simulate(network, {haul(1, 0.0)}, planner);
        REQUIRE(trips.size() == 1);
        const TimedTrip& t = trips[0];
        REQUIRE(t.completed);
        REQUIRE(t.stops.size() == 1);

        const Stop& rest = t.stops[0];
        CHECK(rest.levelAfter == HeavyVehicleStandardHours::kMaxWorkHours);
        CHECK(rest.energyCost == 0.0);
        CHECK_THAT(rest.serviceHours,
                   WithinAbs(SimulatorConfig{}.stopOverheadHours + HeavyVehicleStandardHours::kMajorRestHours, 1e-12));

        // Hours left at the port = 12 after the rest, minus the time driven since.
        Km afterRest = 0.0;
        for (NodeId n = rest.node; n != 4; ++n) afterRest += network.neighbours(n).back().distanceKm;
        CHECK_THAT(t.levelAtFinish, WithinRel(12.0 - afterRest / SimulatorConfig{}.speedKmh, 1e-12));
        CHECK_THAT(t.drivingHours, WithinRel(1100.0 / SimulatorConfig{}.speedKmh, 1e-12));
    }
}

TEST_CASE("when the bays run out, a tired driver cannot legally go on", "[truck]") {
    // Two drivers leave together, each having already worked 8 of their 12
    // hours: 4 hours, 320 km, left. Both can reach R1 (300 km) and neither can
    // reach R2 (550 km), so both must rest at R1. With two bays both rest and
    // both deliver. With one, the second is turned away with 0.25h -- 20 km --
    // on the clock, and R2 is 250 km on. That is the parking shortage: not a
    // delay, a driver with no legal way to continue.
    const std::vector<Demand> pair{haul(1, 8.0), haul(2, 8.0)};
    for (const std::string& planner : plannerNames()) {
        INFO("planner " << planner);

        const auto enough = simulate(restAreaCorridor(2), pair, planner);
        CHECK(enough[0].completed);
        CHECK(enough[1].completed);
        CHECK(enough[0].turnedAway + enough[1].turnedAway == 0);

        const auto short_ = simulate(restAreaCorridor(1), pair, planner);
        CHECK(short_[0].completed != short_[1].completed);
        const TimedTrip& unlucky = short_[0].completed ? short_[1] : short_[0];
        CHECK(unlucky.turnedAway == 1);
        CHECK(unlucky.stops.empty());
    }
}

TEST_CASE("a dataset's domain name builds the right domain, at the simulator's speed", "[truck][domain]") {
    CHECK(makeDomain("ev", 80.0).get() == electricVehicle().get());

    const auto t = makeDomain("truck", 90.0);
    CHECK(t->name() == "truck");
    CHECK(t->admission() == Admission::TurnAway);
    // The speed handed in is the speed the clock runs at.
    CHECK_THAT(t->resourceForDistance(90.0, kAllDrivingIsWork), WithinAbs(1.0, 1e-12));

    CHECK_THROWS_AS(makeDomain("lorry", 80.0), std::invalid_argument);
    for (const auto& name : domainNames()) CHECK_NOTHROW(makeDomain(name, 80.0));
}

TEST_CASE("a rest area is not faulted for having no charging power", "[truck]") {
    // The validator's concern is a station that serves in no time. For an EV,
    // zero power means exactly that; for a driver, the rest takes seven hours
    // whatever the rate column says.
    Network ev = restAreaCorridor(2);
    ev.setDomain(electricVehicle());
    const Network truck_ = restAreaCorridor(2);
    const auto mentionsPower = [](const std::vector<std::string>& warnings) {
        for (const auto& w : warnings) {
            if (w.find("charging power") != std::string::npos) return true;
        }
        return false;
    };
    CHECK(mentionsPower(ev.validate()));
    CHECK_FALSE(mentionsPower(truck_.validate()));
}
