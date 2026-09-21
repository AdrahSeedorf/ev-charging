#pragma once

// Laws every Domain must obey, written once and run against each one.
//
// These are not tests of the EV physics. They are the assumptions the engine
// makes about ANY domain without saying so -- found by reading what the router,
// the planners and the simulator do with the numbers a domain returns. A new
// domain that breaks one of these will not fail loudly; it will produce plans
// that look reasonable and are wrong. So each law says which part of the engine
// depends on it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <vector>

#include "evnet/domain.hpp"

namespace evnet::testing {

inline void checkDomainLaws(const Domain& domain, PerDistance consumption, Rate rate) {
    using Catch::Matchers::WithinAbs;
    using Catch::Matchers::WithinRel;
    const std::vector<Km> distances{0.0, 0.5, 12.0, 57.0, 140.0, 891.0};

    INFO("domain " << domain.name() << ", consumption " << consumption << ", rate " << rate);

    // Law 1: standing still is free. The optimal planner's zero-length
    // self-transition and every "service in place" candidate rely on it.
    CHECK_THAT(domain.resourceForDistance(0.0, consumption), WithinAbs(0.0, 1e-12));

    for (const Km d : distances) {
        const Resource cost = domain.resourceForDistance(d, consumption);

        // Law 2: travel never pays you. A negative cost would let an agent gain
        // resource by driving, and reachableWithin would never terminate its search.
        CHECK(cost >= 0.0);

        // Law 3: longer is never cheaper. Router's range-limited Dijkstra prunes on
        // this; without it, "out of range" would not mean "further ones are too".
        CHECK(domain.resourceForDistance(d + 1.0, consumption) >= cost);

        // Law 4: distance and resource convert both ways without drift. The
        // engine decides reachability with one direction and charges for the leg
        // with the other; a gap is an agent that plans a leg it cannot then drive.
        if (d > 0.0) {
            CHECK_THAT(domain.distanceOnResource(cost, consumption), WithinRel(d, 1e-9));
        }
    }

    // Law 5: cost is additive over legs. The simulator deducts resource leg by
    // leg while canFinish prices the whole remaining distance in one call; the
    // two agree only if splitting a journey does not change what it costs.
    for (const Km a : distances) {
        for (const Km b : distances) {
            const Resource whole = domain.resourceForDistance(a + b, consumption);
            const Resource parts =
                domain.resourceForDistance(a, consumption) + domain.resourceForDistance(b, consumption);
            CHECK_THAT(whole, WithinAbs(parts, 1e-9 * (1.0 + whole)));
        }
    }

    // Law 6: service takes no time for nothing, never negative time, and never
    // less time for more. Both congestion engines schedule a server as busy for
    // exactly this long.
    CHECK_THAT(domain.serviceDuration(0.0, rate), WithinAbs(0.0, 1e-12));
    Hours previous = 0.0;
    for (const Resource amount : {0.1, 1.0, 5.0, 20.0, 60.0}) {
        const Hours h = domain.serviceDuration(amount, rate);
        CHECK(h >= 0.0);
        CHECK(h >= previous);
        previous = h;
    }

    // Laws 7-10 concern what a stop delivers, for an agent of this capacity
    // arriving at any level and asking for any higher one.
    const Resource capacity = 60.0;
    for (const double arriveFraction : {0.0, 0.1, 0.35, 0.8, 1.0}) {
        const Resource arrival = capacity * arriveFraction;
        for (const double askFraction : {0.0, 0.2, 0.5, 1.0}) {
            const Resource requested = arrival + (capacity - arrival) * askFraction;
            const Resource after = domain.levelAfterService(arrival, requested, capacity);
            INFO("arrive " << arrival << ", ask " << requested << ", leave " << after);

            // Law 7: service never takes resource away.
            CHECK(after >= arrival);
            // Law 8: service never overfills. Planners clamp requests to capacity
            // and assume the result respects it.
            CHECK(after <= capacity);
            // Law 9: service never delivers less than was asked. Every planner
            // plans its next leg from the level it requested; less than that is an
            // agent that leaves unable to do what it stopped in order to do.
            CHECK(after >= requested);
            // Law 10: asking again for what you were given changes nothing. A
            // planner asks for the level the domain told it it would get, and the
            // simulator then applies the domain once more; the two agree only if
            // service is idempotent.
            CHECK(domain.levelAfterService(arrival, after, capacity) == after);
        }
    }
}

}  // namespace evnet::testing
