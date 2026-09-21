#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "evnet/candidates.hpp"
#include "evnet/domain.hpp"
#include "evnet/planner.hpp"
#include "evnet/simulation.hpp"
#include "evnet/simulator.hpp"
#include "evnet/station_runtime.hpp"
#include "evnet/station_state.hpp"
#include "fixtures.hpp"

// Admission: what a full station does with an arrival.
//
// EV chargers have somewhere to wait; truck parking does not. These tests use
// EV physics with turn-away admission, so that the ONLY difference from the
// shipped behaviour is the one under test.

using namespace evnet;
using Catch::Matchers::WithinAbs;

namespace {

class TurnsAway final : public Domain {
public:
    std::string name() const override { return "ev-turn-away"; }
    Resource resourceForDistance(Km d, PerDistance c) const override { return ev_->resourceForDistance(d, c); }
    Km distanceOnResource(Resource r, PerDistance c) const override { return ev_->distanceOnResource(r, c); }
    Hours serviceDuration(Resource a, Rate r) const override { return ev_->serviceDuration(a, r); }
    Admission admission() const override { return Admission::TurnAway; }
    Resource levelAfterService(Resource l, Resource r, Resource c) const override {
        return ev_->levelAfterService(l, r, c);
    }

private:
    std::shared_ptr<const Domain> ev_ = electricVehicle();
};

Network turningAway(Network network) {
    network.setDomain(std::make_shared<TurnsAway>());
    return network;
}

std::vector<TimedTrip> simulate(const Network& network, const std::vector<Demand>& demands,
                                const std::string& planner) {
    const Router router(network);
    const SimulatorConfig config;
    const auto p = makePlanner(planner, network, router, config.valueOfTimePerHour, config.feasibility());
    const Simulator simulator(network, router, config);
    StationRuntime runtime(network, config.stopOverheadHours);
    return simulator.run(demands, *p, runtime);
}

struct Loaded {
    Network network;
    std::vector<Demand> demands;
};

Loaded hume() {
    const std::string root = std::string(EVNET_PROJECT_ROOT) + "/data/hume/";
    return {Network::load(root + "nodes.csv", root + "edges.csv"), Demand::load(root + "demands.csv")};
}

}  // namespace

TEST_CASE("a full station that queues makes you wait; one that turns away refuses you", "[admission]") {
    // Mid1 (node 1) has one charger at 100 kW. Occupy it from 0 to 0.3h.
    const Network queues = testing::corridor();
    const Network refuses = turningAway(testing::corridor());

    StationRuntime q(queues, 0.0);
    StationRuntime r(refuses, 0.0);
    q.admit(1, 1, 0.0, 30.0);
    r.admit(1, 1, 0.0, 30.0);

    SECTION("while it is busy") {
        CHECK_THAT(q.expectedWait(1, 0.1), WithinAbs(0.2, 1e-12));
        CHECK(r.expectedWait(1, 0.1) == kNoAdmission);
    }
    SECTION("the moment it frees up, both let you in") {
        CHECK(q.expectedWait(1, 0.3) == 0.0);
        CHECK(r.expectedWait(1, 0.3) == 0.0);
        CHECK(r.admit(1, 2, 0.3, 10.0).wait() == 0.0);
    }
    SECTION("admitting into a full turn-away station is a bug upstream, not a queue") {
        CHECK(q.admit(1, 2, 0.1, 10.0).wait() > 0.0);
        CHECK_THROWS_AS(r.admit(1, 2, 0.1, 10.0), std::logic_error);
    }
}

TEST_CASE("turned away with too little left to reach the next stop", "[admission]") {
    // Two drivers set off together with 25 kWh: enough for Mid1 (100 km) but not
    // Mid2 (200 km), so both must stop at Mid1, which has one charger. Where the
    // station queues, the second waits and both finish. Where it turns arrivals
    // away, the second is refused with 7 kWh -- 39 km of range -- and Mid2 is 100
    // km on. That is the truck-parking failure in miniature: not a long wait, but
    // no legal way forward.
    Demand first = testing::corridorJourney(25.0);
    Demand second = first;
    second.id = 2;
    const std::vector<Demand> both{first, second};

    for (const std::string& planner : plannerNames()) {
        INFO("planner " << planner);

        const auto queued = simulate(testing::corridor(), both, planner);
        REQUIRE(queued.size() == 2);
        CHECK(queued[0].completed);
        CHECK(queued[1].completed);
        CHECK(queued[0].turnedAway + queued[1].turnedAway == 0);
        const Hours waited = queued[0].waitHours + queued[1].waitHours;
        CHECK(waited > 0.0);  // the scenario genuinely contends

        const auto refused = simulate(turningAway(testing::corridor()), both, planner);
        REQUIRE(refused.size() == 2);
        CHECK(refused[0].completed != refused[1].completed);  // exactly one gets through
        const TimedTrip& unlucky = refused[0].completed ? refused[1] : refused[0];
        const TimedTrip& lucky = refused[0].completed ? refused[0] : refused[1];
        CHECK(unlucky.turnedAway == 1);
        CHECK(lucky.turnedAway == 0);
        CHECK(lucky.waitHours == 0.0);
        CHECK(unlucky.stops.empty());
        CHECK_FALSE(unlucky.failure.empty());
    }
}

TEST_CASE("a driver below reserve at a full station may drive on", "[admission]") {
    // The optimal planner forbids driving on from a station while below the
    // reserve, because the reason for waiving the reserve there is that you are
    // about to charge. At a full station that refuses you, you are not -- and
    // forbidding the drive would strand the very driver being turned away.
    //
    //   Start --100-- A (1 bay) --20-- B (1 bay) --100-- Target
    //
    // 60 kWh at 20 kWh/100km: the reserve is 6 kWh. The agent is at A with 5.
    Network network;
    Node start;
    start.name = "Start";
    network.addNode(start);
    Node a;
    a.name = "A";
    a.station = Station{0.45, 1, 50.0};
    network.addNode(a);
    Node b;
    b.name = "B";
    b.station = Station{0.45, 1, 50.0};
    network.addNode(b);
    Node target;
    target.name = "Target";
    network.addNode(target);
    network.addEdge(0, 1, 100.0);
    network.addEdge(1, 2, 20.0);
    network.addEdge(2, 3, 100.0);

    AgentState agent;
    agent.id = 7;
    agent.at = 1;
    agent.destination = 3;
    agent.level = 5.0;
    agent.capacity = 60.0;
    agent.consumption = 20.0;
    agent.now = 0.1;

    // No SECTIONs here: Catch2 enters each named section once per pass of the
    // test case, so sections inside a loop over planners run for the first
    // planner only -- and the planner this rule lives in is the last one.
    const Network refusing = turningAway(network);
    const SimulatorConfig config;
    for (const std::string& planner : plannerNames()) {
        INFO("planner " << planner);
        {
            // Where A queues, the agent has somewhere to charge: any move is legal.
            const Router router(network);
            const auto p = makePlanner(planner, network, router, config.valueOfTimePerHour, config.feasibility());
            StationRuntime runtime(network, config.stopOverheadHours);
            runtime.admit(1, 99, 0.0, 30.0);  // someone is on A's only charger
            CHECK(p->decide(agent, runtime).kind != Action::Kind::Infeasible);
        }
        {
            // Where A turns arrivals away, the only way forward is B.
            const Router router(refusing);
            const auto p = makePlanner(planner, refusing, router, config.valueOfTimePerHour, config.feasibility());
            StationRuntime runtime(refusing, config.stopOverheadHours);
            runtime.admit(1, 99, 0.0, 30.0);
            const Action action = p->decide(agent, runtime);
            CHECK(action.kind == Action::Kind::DriveToStation);
            CHECK(action.target == 2);
        }
    }
}

TEST_CASE("a top-up is never offered a station that would refuse it", "[admission]") {
    // Top-up missions take a separate path through the candidate builder.
    // From Start with 45 kWh, a 30 kWh top-up is feasible at Mid1 (100 km, 1.25h
    // away) and at Mid2 (200 km: 9 kWh left on arrival, 39 after, 36 to get
    // home). Mid1's only charger is busy until 1.6h, so on arrival it is full: a
    // queueing Mid1 is still an option, a turn-away Mid1 is not.
    const Network queues = testing::corridor();
    const Network refuses = turningAway(testing::corridor());
    AgentState agent;
    agent.at = 0;
    agent.destination = 0;
    agent.level = 45.0;
    agent.capacity = 60.0;
    agent.consumption = 18.0;

    const auto offered = [&](const Network& network) {
        const Router router(network);
        StationRuntime runtime(network, 0.1);
        runtime.admit(1, 99, 0.0, 150.0);  // Mid1 busy until 0.1 + 150/100 = 1.6h
        std::vector<NodeId> nodes;
        for (const Candidate& c : buildTopUpCandidates(network, router, runtime, agent, 30.0, FeasibilityConfig{})) {
            nodes.push_back(c.node);
        }
        return nodes;
    };
    const auto q = offered(queues);
    const auto r = offered(refuses);
    CHECK(std::find(q.begin(), q.end(), 1) != q.end());
    CHECK(std::find(r.begin(), r.end(), 1) == r.end());
    CHECK(std::find(r.begin(), r.end(), 2) != r.end());
}

TEST_CASE("on a busy real network, nobody waits at a station that turns arrivals away", "[admission]") {
    const Loaded queued = hume();
    Loaded refused = hume();
    refused.network = turningAway(refused.network);

    bool anyoneWaitedWhereStationsQueue = false;
    for (const std::string& planner : plannerNames()) {
        INFO("planner " << planner);
        for (const TimedTrip& t : simulate(queued.network, queued.demands, planner)) {
            CHECK(t.turnedAway == 0);  // EV never turns anyone away
            if (t.waitHours > 0.0) anyoneWaitedWhereStationsQueue = true;
        }
        // No exception from admit() means no agent was ever let into a full
        // turn-away station; no wait means none was made to queue at one.
        for (const TimedTrip& t : simulate(refused.network, refused.demands, planner)) {
            CHECK(t.waitHours == 0.0);
            for (const Stop& s : t.stops) CHECK(s.waitHours == 0.0);
        }
    }
    CHECK(anyoneWaitedWhereStationsQueue);  // or this test would prove nothing
}

TEST_CASE("the static engine stops assigning to a turn-away station once it is full", "[admission]") {
    // The timeless engine never releases anyone, so a turn-away station closes
    // for good once `servers` agents have been assigned to it.
    const Loaded queued = hume();
    Loaded refused = hume();
    refused.network = turningAway(refused.network);

    const SimulationConfig config;
    const auto policy = makePolicy("generalised", config.valueOfTimePerHour);

    const Router rq(queued.network);
    StationState sq(queued.network);
    Allocator(queued.network, rq, config).run(queued.demands, *policy, sq);

    const Router rr(refused.network);
    StationState sr(refused.network);
    Allocator(refused.network, rr, config).run(refused.demands, *policy, sr);

    bool anyOverCapacityWhenQueueing = false;
    for (const NodeId id : refused.network.stationNodes()) {
        const int servers = refused.network.node(id).station->servers;
        CHECK(sr.queueLength(id) <= servers);
        if (sq.queueLength(id) > servers) anyOverCapacityWhenQueueing = true;
    }
    CHECK(anyOverCapacityWhenQueueing);
}
