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
}

}  // namespace evnet::testing
