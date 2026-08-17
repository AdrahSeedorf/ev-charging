#include <catch2/catch_test_macros.hpp>

#include "evnet/siting.hpp"
#include "fixtures.hpp"

using namespace evnet;

namespace {

/// A corridor with a gap: Start -- Mid1 -- Gap -- Mid2 -- Target, where Gap has
/// no station. Vehicles that cannot bridge Mid1 to Mid2 in one hop are stranded
/// unless a station appears at Gap, so the correct siting answer is known in
/// advance.
Network corridorWithGap() {
    Network network;
    for (const char* name : {"Start", "Mid1", "Gap", "Mid2", "Target"}) {
        Node node;
        node.name = name;
        network.addNode(node);
    }
    network.setStation(1, Station{0.30, 4, 100.0});
    network.setStation(3, Station{0.30, 4, 100.0});
    network.addEdge(0, 1, 100.0);
    network.addEdge(1, 2, 100.0);
    network.addEdge(2, 3, 100.0);
    network.addEdge(3, 4, 100.0);
    return network;
}

Demand gapJourney() {
    Demand demand;
    demand.id = 1;
    demand.origin = 0;
    demand.destination = 4;
    demand.batteryKwh = 30.0;  // 166 km full range, so 200 km hops are impossible
    demand.socKwh = 20.0;
    demand.efficiency = 18.0;
    return demand;
}

}  // namespace

TEST_CASE("siting finds the site that unblocks stranded vehicles", "[siting]") {
    const Network network = corridorWithGap();
    const std::vector<Demand> fleet(5, gapJourney());
    const CheapestEnergyPolicy policy;
    const Siting siting(network);

    const Summary base = siting.baseline(fleet, policy);
    REQUIRE(base.stranded == 5);  // nothing can bridge the gap today

    const auto ranked = siting.rank(fleet, policy, Station{0.40, 4, 150.0}, 0);
    REQUIRE_FALSE(ranked.empty());

    // Ranking is completed-trips first, so the site that rescues the fleet leads.
    CHECK(ranked.front().name == "Gap");
    CHECK(ranked.front().completed == 5);
    CHECK(ranked.front().adoptionShare > 0.0);
}

TEST_CASE("every candidate site is scored exactly once", "[siting]") {
    const Network network = corridorWithGap();
    const std::vector<Demand> fleet(3, gapJourney());
    const CheapestEnergyPolicy policy;
    const Siting siting(network);

    const auto all = siting.rank(fleet, policy, Station{0.40, 4, 150.0}, 0);
    CHECK(all.size() == network.candidateSites().size());

    const auto topOne = siting.rank(fleet, policy, Station{0.40, 4, 150.0}, 1);
    CHECK(topOne.size() == 1);
    CHECK(topOne.front().name == all.front().name);
}

TEST_CASE("siting accounts for the congestion a new station creates", "[siting]") {
    // Two candidate sites are equally well placed, but the prototype station has
    // a single charger. The legacy implementation ignored queues entirely and so
    // could not distinguish a well-placed site from a well-placed bottleneck;
    // here the queue the new station attracts shows up in its own score.
    const Network network = corridorWithGap();
    const std::vector<Demand> fleet(20, gapJourney());
    const CheapestEnergyPolicy policy;
    const Siting siting(network);

    const auto oneCharger = siting.rank(fleet, policy, Station{0.40, 1, 150.0}, 0);
    const auto manyChargers = siting.rank(fleet, policy, Station{0.40, 20, 150.0}, 0);

    REQUIRE(oneCharger.front().name == "Gap");
    REQUIRE(manyChargers.front().name == "Gap");

    // Same location, same fleet, same price -- only the capacity differs, and the
    // scoring must reflect that.
    CHECK(oneCharger.front().meanWaitHours > manyChargers.front().meanWaitHours);
    CHECK(oneCharger.front().meanGeneralisedCost > manyChargers.front().meanGeneralisedCost);
}

TEST_CASE("a network with no candidate sites yields no recommendations", "[siting]") {
    Network network = corridorWithGap();
    for (const NodeId id : network.candidateSites()) {
        network.setStation(id, Station{0.50, 2, 50.0});
    }
    REQUIRE(network.candidateSites().empty());

    const Siting siting(network);
    const std::vector<Demand> fleet(2, gapJourney());
    CHECK(siting.rank(fleet, CheapestEnergyPolicy(), Station{0.40, 4, 150.0}, 0).empty());
}

TEST_CASE("siting is stable across repeated runs", "[siting]") {
    const Network network = corridorWithGap();
    const std::vector<Demand> fleet(8, gapJourney());
    const CheapestEnergyPolicy policy;
    const Siting siting(network);

    const auto first = siting.rank(fleet, policy, Station{0.40, 3, 150.0}, 0);
    const auto second = siting.rank(fleet, policy, Station{0.40, 3, 150.0}, 0);

    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i].node == second[i].node);
        CHECK(first[i].meanGeneralisedCost == second[i].meanGeneralisedCost);
    }
}

TEST_CASE("timed siting finds the same unblocking site, on the clock", "[siting]") {
    // Siting was the last command still answering from stage 1's timeless tally: it
    // never dispatched on --engine, so the toolkit's headline question was being
    // decided by the model stage 2 replaced. The known-answer case must come out the
    // same under both engines, or one of them is wrong.
    const Network network = corridorWithGap();
    const std::vector<Demand> fleet(5, gapJourney());
    const Siting siting(network, SimulatorConfig{});

    const TimedSummary base = siting.baselineTimed(fleet, "cheapest");
    REQUIRE(base.stranded == 5);

    const auto ranked = siting.rankTimed(fleet, "cheapest", Station{0.40, 4, 150.0}, 0);
    REQUIRE_FALSE(ranked.empty());
    CHECK(ranked.front().name == "Gap");
    CHECK(ranked.front().completed == 5);
    CHECK(ranked.front().adoptionShare > 0.0);
}

TEST_CASE("a station no vehicle uses changes nothing at all", "[siting]") {
    // The sharpest statement of what a siting delta means: it must be the consequence of
    // vehicles CHOOSING the new station and nothing else. So if uptake is zero the score
    // must equal the baseline exactly -- not nearly, exactly. Anything else means the
    // mere existence of a station is moving the model, and every ranking is then part
    // artefact.
    //
    // Worth stating as a test because the appearance of violating it is so convincing.
    // Real deltas of 10-30 cents sat next to an uptake column reading "0%", which looked
    // exactly like a leak; the uptake was really 0.25% and the column was rounding it
    // away. The property held the whole time. What the hunt did turn up was a genuine
    // reserve-waiver bug in the optimal planner, found while proving this.
    const Network network = corridorWithGap();
    std::vector<Demand> fleet;
    for (int i = 0; i < 6; ++i) {
        Demand demand = gapJourney();
        demand.id = i;
        demand.batteryKwh = 60.0;  // ample, so the gap is not what decides the outcome
        demand.socKwh = 40.0;
        demand.releaseHour = static_cast<Hours>(i) * 0.2;
        fleet.push_back(demand);
    }

    const Station unusable{999.0, 4, 150.0};

    SECTION("under the event-driven clock") {
        const Siting siting(network, SimulatorConfig{});
        const TimedSummary base = siting.baselineTimed(fleet, "generalised");
        const auto ranked = siting.rankTimed(fleet, "generalised", unusable, 0);

        REQUIRE_FALSE(ranked.empty());
        for (const auto& score : ranked) {
            CHECK(score.adoptionShare == 0.0);
            CHECK(score.completed == base.completed);
            CHECK(score.meanGeneralisedCost == base.meanGeneralisedCost);
            CHECK(score.meanMoneyCost == base.meanMoneyCost);
            CHECK(score.meanWaitHours == base.meanWaitHours);
            CHECK(score.peakQueue == base.peakWaiting);
        }
    }

    SECTION("and under the static tally") {
        // The deprecated engine is held to the same standard. It is still selectable, so
        // it can still be believed, and a deprecated model that quietly lies is worse
        // than one that is simply coarse.
        const Siting siting(network);
        const GeneralisedCostPolicy policy(20.0);
        const Summary base = siting.baseline(fleet, policy);
        const auto ranked = siting.rank(fleet, policy, unusable, 0);

        REQUIRE_FALSE(ranked.empty());
        for (const auto& score : ranked) {
            CHECK(score.adoptionShare == 0.0);
            CHECK(score.completed == base.completed);
            CHECK(score.meanGeneralisedCost == base.meanGeneralisedCost);
            CHECK(score.meanWaitHours == base.meanWaitHours);
        }
    }
}
