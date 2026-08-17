#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "evnet/policy.hpp"

using namespace evnet;
using Catch::Matchers::WithinAbs;

namespace {

/// Three options that disagree on every axis, so each policy must pick a
/// different one. This is the whole justification for keeping four policies:
/// if they always agreed, the comparison would be vacuous.
std::vector<Candidate> divergentOptions() {
    Candidate cheap;  // cheapest energy, but a long queue and little progress
    cheap.node = 1;
    cheap.progressKm = 10.0;
    cheap.energyKwh = 20.0;
    cheap.travelCost = 1.0;
    cheap.energyCost = 4.0;
    cheap.waitHours = 3.0;
    cheap.chargeHours = 0.5;

    Candidate quiet;  // no queue at all, but dear
    quiet.node = 2;
    quiet.progressKm = 30.0;
    quiet.energyKwh = 20.0;
    quiet.travelCost = 3.0;
    quiet.energyCost = 14.0;
    quiet.waitHours = 0.0;
    quiet.chargeHours = 0.5;

    Candidate distant;  // furthest along the route, middling on both other axes
    distant.node = 3;
    distant.progressKm = 60.0;
    distant.energyKwh = 20.0;
    distant.travelCost = 5.0;
    distant.energyCost = 9.0;
    distant.waitHours = 1.0;
    distant.chargeHours = 0.5;

    return {cheap, quiet, distant};
}

}  // namespace

TEST_CASE("generalised cost combines money and time", "[policy]") {
    Candidate candidate;
    candidate.travelCost = 2.0;
    candidate.energyCost = 8.0;
    candidate.waitHours = 1.5;
    candidate.chargeHours = 0.5;

    CHECK_THAT(candidate.moneyCost(), WithinAbs(10.0, 1e-9));
    CHECK_THAT(candidate.timeHours(), WithinAbs(2.0, 1e-9));
    CHECK_THAT(candidate.generalisedCost(0.0), WithinAbs(10.0, 1e-9));
    CHECK_THAT(candidate.generalisedCost(20.0), WithinAbs(50.0, 1e-9));
}

TEST_CASE("each policy optimises its own axis", "[policy]") {
    const auto options = divergentOptions();

    SECTION("cheapest ignores queues") {
        const CheapestEnergyPolicy policy;
        const Candidate* choice = policy.choose(options);
        REQUIRE(choice != nullptr);
        CHECK(choice->node == 1);  // $5 total, despite a 3 hour wait
    }
    SECTION("min-wait ignores price") {
        const MinWaitPolicy policy;
        const Candidate* choice = policy.choose(options);
        REQUIRE(choice != nullptr);
        CHECK(choice->node == 2);  // no queue, despite costing $17
    }
    SECTION("farthest maximises progress") {
        const FarthestReachablePolicy policy;
        const Candidate* choice = policy.choose(options);
        REQUIRE(choice != nullptr);
        CHECK(choice->node == 3);  // 60 km of progress
    }
}

TEST_CASE("the value of time decides what generalised cost prefers", "[policy]") {
    const auto options = divergentOptions();

    SECTION("time worth nothing reduces to the cheapest policy") {
        const GeneralisedCostPolicy policy(0.0);
        const Candidate* choice = policy.choose(options);
        REQUIRE(choice != nullptr);
        CHECK(choice->node == 1);
    }
    SECTION("time worth a great deal reduces to the min-wait policy") {
        const GeneralisedCostPolicy policy(1000.0);
        const Candidate* choice = policy.choose(options);
        REQUIRE(choice != nullptr);
        CHECK(choice->node == 2);
    }
    SECTION("a moderate value picks the compromise neither extreme would") {
        // At $20/h: cheap = 5 + 70 = 75, quiet = 17 + 10 = 27, distant = 14 + 30 = 44.
        // The interesting property is that the ordering genuinely flips with the
        // parameter -- this is the trade-off neither legacy project could express.
        const GeneralisedCostPolicy policy(20.0);
        const Candidate* choice = policy.choose(options);
        REQUIRE(choice != nullptr);
        CHECK(choice->node == 2);
    }
    SECTION("the crossover between two options can be located") {
        // cheap: 5 + 3.5v   distant: 14 + 1.5v   equal at v = 4.5
        std::vector<Candidate> pair{divergentOptions()[0], divergentOptions()[2]};
        CHECK(GeneralisedCostPolicy(4.0).choose(pair)->node == 1);
        CHECK(GeneralisedCostPolicy(5.0).choose(pair)->node == 3);
    }
}

TEST_CASE("policies are deterministic under ties", "[policy]") {
    // A comparison against a coin flip measures nothing, so ties must resolve the
    // same way every run. Lowest node id wins.
    Candidate a;
    a.node = 5;
    a.energyCost = 10.0;
    Candidate b = a;
    b.node = 2;
    Candidate c = a;
    c.node = 9;

    const CheapestEnergyPolicy policy;
    for (int i = 0; i < 20; ++i) {
        CHECK(policy.choose({a, b, c})->node == 2);
        CHECK(policy.choose({c, b, a})->node == 2);
    }
}

TEST_CASE("an empty candidate list yields no choice", "[policy]") {
    const std::vector<Candidate> none;
    CHECK(CheapestEnergyPolicy().choose(none) == nullptr);
    CHECK(MinWaitPolicy().choose(none) == nullptr);
    CHECK(FarthestReachablePolicy().choose(none) == nullptr);
    CHECK(GeneralisedCostPolicy(20.0).choose(none) == nullptr);
}

TEST_CASE("the factory covers every advertised policy", "[policy]") {
    for (const auto& name : policyNames()) {
        const auto policy = makePolicy(name, 20.0);
        REQUIRE(policy != nullptr);
        CHECK(policy->name() == name);
    }
    // Both spellings of the merged policy are accepted.
    CHECK(makePolicy("generalized", 20.0)->name() == "generalised");
    CHECK_THROWS_AS(makePolicy("nonsense", 20.0), std::invalid_argument);
}
