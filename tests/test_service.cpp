#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "domain_laws.hpp"
#include "evnet/domain.hpp"
#include "evnet/planner.hpp"
#include "evnet/simulation.hpp"
#include "evnet/simulator.hpp"
#include "evnet/station_runtime.hpp"
#include "evnet/station_state.hpp"

// What a stop delivers is the domain's to decide.
//
// An EV driver chooses how much to charge. A truck driver taking a mandated
// rest does not choose how much of their clock comes back: all of it does. These
// tests use EV physics with an all-or-nothing reset, so that the only difference
// from the shipped behaviour is the one under test.

using namespace evnet;
using Catch::Matchers::WithinAbs;

namespace {

class ResetsToFull final : public Domain {
public:
    std::string name() const override { return "ev-full-reset"; }
    Resource resourceForDistance(Km d, PerDistance c) const override { return ev_->resourceForDistance(d, c); }
    Km distanceOnResource(Resource r, PerDistance c) const override { return ev_->distanceOnResource(r, c); }
    Hours serviceDuration(Resource a, Rate r) const override { return ev_->serviceDuration(a, r); }
    Admission admission() const override { return Admission::Queue; }
    /// Ask for anything and you get everything; ask for nothing and you get nothing.
    Resource levelAfterService(Resource arrival, Resource requested, Resource capacity) const override {
        return requested > arrival ? capacity : arrival;
    }

private:
    std::shared_ptr<const Domain> ev_ = electricVehicle();
};

struct Loaded {
    Network network;
    std::vector<Demand> demands;
};

Loaded load(const std::string& dataset, bool reset) {
    const std::string root = std::string(EVNET_PROJECT_ROOT) + "/data/" + dataset + "/";
    Loaded l{Network::load(root + "nodes.csv", root + "edges.csv"), Demand::load(root + "demands.csv")};
    if (reset) l.network.setDomain(std::make_shared<ResetsToFull>());
    return l;
}

std::map<int, Resource> capacities(const std::vector<Demand>& demands) {
    std::map<int, Resource> out;
    for (const Demand& d : demands) out[d.id] = d.capacity;
    return out;
}

}  // namespace

TEST_CASE("an all-or-nothing reset obeys every domain law", "[service]") {
    testing::checkDomainLaws(ResetsToFull{}, 18.0, 50.0);
}

TEST_CASE("where service resets to full, every stop leaves the agent full", "[service]") {
    // Sydney as well as Hume: only Sydney has top-up missions, which take their
    // own path through both engines.
    for (const std::string dataset : {"hume", "sydney"}) {
        INFO("dataset " << dataset);
        const Loaded plain = load(dataset, false);
        const Loaded reset = load(dataset, true);
        const auto capacity = capacities(reset.demands);
        const SimulatorConfig config;

        bool anyPartialChargeInEv = false;
        std::size_t stops = 0;
        for (const std::string& planner : plannerNames()) {
            INFO("event-driven, planner " << planner);
            for (const auto* l : {&plain, &reset}) {
                const Router router(l->network);
                const auto p = makePlanner(planner, l->network, router, config.valueOfTimePerHour,
                                           config.feasibility());
                StationRuntime runtime(l->network, config.stopOverheadHours);
                for (const TimedTrip& t : Simulator(l->network, router, config).run(l->demands, *p, runtime)) {
                    for (const Stop& s : t.stops) {
                        const Station& st = *l->network.node(s.node).station;
                        // Charged for, and served for, what was delivered -- not
                        // what the planner asked for.
                        CHECK_THAT(s.energyCost, WithinAbs(s.amount * st.pricePerUnit, 1e-9));
                        CHECK_THAT(s.serviceHours,
                                   WithinAbs(config.stopOverheadHours +
                                                 l->network.domain().serviceDuration(s.amount, st.ratePerHour),
                                             1e-9));
                        if (l == &reset) {
                            CHECK(s.levelAfter == capacity.at(t.demandId));
                            ++stops;
                        } else if (s.levelAfter < capacity.at(t.demandId)) {
                            anyPartialChargeInEv = true;
                        }
                    }
                }
            }
        }

        const SimulationConfig staticConfig;
        for (const std::string& policy : policyNames()) {
            INFO("static, policy " << policy);
            const auto p = makePolicy(policy, staticConfig.valueOfTimePerHour);
            const Router router(reset.network);
            StationState state(reset.network);
            for (const TripResult& t : Allocator(reset.network, router, staticConfig).run(reset.demands, *p, state)) {
                for (const Stop& s : t.stops) {
                    CHECK(s.levelAfter == capacity.at(t.demandId));
                    ++stops;
                }
            }
        }

        CHECK(stops > 0);
        CHECK(anyPartialChargeInEv);  // or the reset would be indistinguishable
    }
}

TEST_CASE("the optimal planner prices a reset as the whole reset", "[service]") {
    //   Start --100-- Mid (100 kW, $0.45) --100-- Target
    //
    // 30 kWh at 18 kWh/100km, starting with 20: the agent must stop at Mid. On the
    // planner's grid (0.75 kWh a level) it arrives with 1.5 kWh. An EV buys enough
    // to finish with its 3 kWh reserve -- up to 21 kWh, 19.5 bought. A reset fills
    // it to 30, 28.5 bought. The planner must price the 9 kWh difference, plus the
    // extra service time at its value of time: if it asks the domain nothing, it
    // plans a charge the station will not sell and the difference is zero.
    Network network;
    Node start;
    start.name = "Start";
    network.addNode(start);
    Node mid;
    mid.name = "Mid";
    mid.station = Station{0.45, 2, 100.0};
    network.addNode(mid);
    Node target;
    target.name = "Target";
    network.addNode(target);
    network.addEdge(0, 1, 100.0);
    network.addEdge(1, 2, 100.0);

    Network resetting = network;
    resetting.setDomain(std::make_shared<ResetsToFull>());

    AgentState agent;
    agent.id = 1;
    agent.at = 0;
    agent.destination = 2;
    agent.level = 20.0;
    agent.capacity = 30.0;
    agent.consumption = 18.0;

    const SimulatorConfig config;
    const auto cost = [&](const Network& n) {
        const Router router(n);
        const OptimalPlanner planner(n, router, config.valueOfTimePerHour, config.feasibility());
        StationRuntime runtime(n, config.stopOverheadHours);
        return planner.planCost(agent, runtime);
    };
    const Dollars extraEnergy = 9.0 * 0.45;
    const Dollars extraTime = (28.5 - 19.5) / 100.0 * config.valueOfTimePerHour;
    CHECK_THAT(cost(resetting) - cost(network), WithinAbs(extraEnergy + extraTime, 1e-9));
}
