// Tests against the datasets actually shipped in data/. These are the regression
// tests that keep the two legacy projects honest: the Sydney metro graph and the
// Hume corridor both have to load, validate, and behave the way their source
// projects implied.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>

#include "evnet/network.hpp"
#include "evnet/router.hpp"
#include "evnet/siting.hpp"
#include "evnet/simulation.hpp"

using namespace evnet;
using Catch::Matchers::WithinAbs;

namespace {

const std::string kRoot = EVNET_PROJECT_ROOT;

Network load(const std::string& name) {
    return Network::load(kRoot + "/data/" + name + "/nodes.csv",
                         kRoot + "/data/" + name + "/edges.csv");
}

std::vector<Demand> demands(const std::string& name) {
    return Demand::load(kRoot + "/data/" + name + "/demands.csv");
}

}  // namespace

TEST_CASE("the Sydney metro dataset loads and validates clean", "[data]") {
    const Network network = load("sydney");
    CHECK(network.size() == 24);
    CHECK(network.stationNodes().size() == 16);
    CHECK(network.candidateSites().size() == 8);

    // The raw legacy matrix was asymmetric in five places and one value short.
    // Those defects are corrected in tools/build_datasets.py, so the shipped
    // dataset must now be structurally sound.
    const auto warnings = network.validate();
    CHECK(warnings.empty());
}

TEST_CASE("Sydney distances are symmetric after the data corrections", "[data]") {
    const Network network = load("sydney");
    const Router router(network);
    for (NodeId a = 0; a < static_cast<NodeId>(network.size()); ++a) {
        for (NodeId b = a + 1; b < static_cast<NodeId>(network.size()); ++b) {
            const Km forward = router.distance(a, b);
            const Km reverse = router.distance(b, a);
            REQUIRE(forward != Router::kUnreachable);
            CHECK_THAT(forward, WithinAbs(reverse, 1e-9));
        }
    }
}

TEST_CASE("the corrected Macquarie Park edge lands where geography says it should",
          "[data]") {
    const Network network = load("sydney");
    const Router router(network);

    const NodeId macquarie = network.findByName("Macquarie Pk");
    const NodeId ryde = network.findByName("Ryde");
    const NodeId campbelltown = network.findByName("Campbelltown");
    REQUIRE(macquarie != kNoNode);
    REQUIRE(ryde != kNoNode);
    REQUIRE(campbelltown != kNoNode);

    // The raw file recorded 'Macquarie Pk -> Campbelltown = 5.2 km', which is
    // roughly a 50 km drive in reality. The value belonged one column later, as
    // the mirror of the correct 'Ryde -> Macquarie Pk = 5.2'.
    CHECK_THAT(router.distance(macquarie, ryde), WithinAbs(5.2, 1e-9));
    CHECK(router.distance(macquarie, campbelltown) > 30.0);
}

TEST_CASE("the Hume corridor dataset loads as a chain", "[data]") {
    const Network network = load("hume");
    CHECK(network.size() == 12);
    // Every corridor town has a charger, so there is nowhere new to site.
    CHECK(network.stationNodes().size() == 12);
    CHECK(network.candidateSites().empty());
    CHECK(network.validate().empty());

    // A chain is just a graph whose interior nodes have two neighbours. The
    // legacy corridor project needed a bespoke prefix-sum distance function for
    // this shape; here it is an ordinary graph.
    CHECK(network.neighbours(0).size() == 1);
    CHECK(network.neighbours(11).size() == 1);
    for (NodeId i = 1; i < 11; ++i) {
        CHECK(network.neighbours(i).size() == 2);
    }
}

TEST_CASE("Sydney to Melbourne matches the legacy corridor total", "[data]") {
    const Network network = load("hume");
    const Router router(network);
    // Sum of the legacy distanceMap: 57+60+83+86+99+115+62+74+87+106+62 = 891.
    CHECK_THAT(router.distance(0, 11), WithinAbs(891.0, 1e-9));
    CHECK(router.path(0, 11).size() == 12);  // the route visits every town
}

TEST_CASE("the corridor fleet completes and congestion concentrates", "[data]") {
    const Network network = load("hume");
    const Router router(network);
    const auto fleet = demands("hume");
    REQUIRE(fleet.size() >= 150);
    REQUIRE(fleet.size() <= 200);

    const Allocator allocator(network, router);
    const MinWaitPolicy policy;
    StationState state(network);
    const auto results = allocator.run(fleet, policy, state);
    const Summary summary = allocator.summarise(results, state, "min-wait");

    CHECK(summary.stranded == 0);
    CHECK(summary.completed == static_cast<int>(fleet.size()));
    // Long-distance vehicles must charge; a fleet that never stops would mean the
    // range-to-energy conversion had gone wrong.
    CHECK(summary.meanStops > 0.0);
    CHECK(summary.peakQueue > 0);
}

TEST_CASE("policies produce measurably different fleet outcomes", "[data]") {
    // If the four policies agreed, keeping both legacy algorithms would be
    // pointless. This asserts the comparison is not vacuous.
    const Network network = load("hume");
    const Router router(network);
    const auto fleet = demands("hume");
    const Allocator allocator(network, router);

    std::vector<Summary> summaries;
    for (const auto& name : policyNames()) {
        const auto policy = makePolicy(name, 20.0);
        StationState state(network);
        const auto results = allocator.run(fleet, *policy, state);
        summaries.push_back(allocator.summarise(results, state, name));
    }
    REQUIRE(summaries.size() == 4);

    const bool waitsDiffer =
        std::any_of(summaries.begin(), summaries.end(), [&](const Summary& s) {
            return std::abs(s.meanWaitHours - summaries.front().meanWaitHours) > 1e-6;
        });
    const bool costsDiffer =
        std::any_of(summaries.begin(), summaries.end(), [&](const Summary& s) {
            return std::abs(s.meanMoneyCost - summaries.front().meanMoneyCost) > 1e-6;
        });
    CHECK(waitsDiffer);
    CHECK(costsDiffer);
}

TEST_CASE("min-wait beats cheapest on peak queue, and loses on price", "[data]") {
    // The central claim of the merge: the two legacy algorithms optimise
    // different things, and each wins on its own axis. If this ever fails, one of
    // them has stopped doing what its source project did.
    const Network network = load("hume");
    const Router router(network);
    const auto fleet = demands("hume");
    const Allocator allocator(network, router);

    StationState cheapState(network);
    const auto cheapSummary = allocator.summarise(
        allocator.run(fleet, CheapestEnergyPolicy(), cheapState), cheapState, "cheapest");

    StationState waitState(network);
    const auto waitSummary =
        allocator.summarise(allocator.run(fleet, MinWaitPolicy(), waitState), waitState, "min-wait");

    CHECK(waitSummary.peakQueue <= cheapSummary.peakQueue);
    CHECK(cheapSummary.meanMoneyCost <= waitSummary.meanMoneyCost);
}

TEST_CASE("the Sydney top-up fleet is served", "[data]") {
    const Network network = load("sydney");
    const Router router(network);
    const auto fleet = demands("sydney");
    REQUIRE(fleet.size() == 100);
    for (const auto& demand : fleet) {
        CHECK(demand.isTopUp());
    }

    const Allocator allocator(network, router);
    const GeneralisedCostPolicy policy(20.0);
    StationState state(network);
    const auto results = allocator.run(fleet, policy, state);
    const Summary summary = allocator.summarise(results, state, "generalised");

    CHECK(summary.completed > 90);  // a handful may want more than any single stop can give
    CHECK(summary.meanStops == 1.0);  // a top-up is exactly one station visit
}

TEST_CASE("siting on Sydney nominates a real candidate and reports an effect", "[data]") {
    const Network network = load("sydney");
    const auto fleet = demands("sydney");
    const GeneralisedCostPolicy policy(20.0);
    const Siting siting(network);

    const auto ranked = siting.rank(fleet, policy, Station{0.45, 4, 150.0}, 3);
    REQUIRE(ranked.size() == 3);
    for (const auto& score : ranked) {
        // Only nodes that lack a station may be recommended.
        CHECK_FALSE(network.node(score.node).hasStation());
        CHECK(score.meanGeneralisedCost > 0.0);
    }
    // The ranking must actually be ordered.
    CHECK(ranked[0].meanGeneralisedCost <= ranked[2].meanGeneralisedCost);
}
