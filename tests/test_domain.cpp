#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "domain_laws.hpp"
#include "evnet/domain.hpp"
#include "evnet/units.hpp"

using namespace evnet;
using Catch::Matchers::WithinAbs;

TEST_CASE("the EV domain obeys every domain law", "[domain]") {
    const auto ev = electricVehicle();
    // The fleet's consumption spans roughly 14-24 kWh/100km; chargers 7-350 kW.
    for (const PerDistance consumption : {14.0, 18.0, 24.0}) {
        for (const Rate rate : {7.0, 50.0, 150.0, 350.0}) {
            testing::checkDomainLaws(*ev, consumption, rate);
        }
    }
}

TEST_CASE("the EV domain is the units.hpp physics, not a second copy of it", "[domain]") {
    const auto ev = electricVehicle();

    SECTION("known values") {
        CHECK_THAT(ev->resourceForDistance(100.0, 18.0), WithinAbs(18.0, 1e-12));
        CHECK_THAT(ev->distanceOnResource(18.0, 18.0), WithinAbs(100.0, 1e-12));
        CHECK_THAT(ev->serviceDuration(50.0, 150.0), WithinAbs(1.0 / 3.0, 1e-12));
    }

    SECTION("identical to the free functions it wraps, across the range the engine uses") {
        for (const Km d : {0.0, 3.7, 57.0, 891.0}) {
            for (const PerDistance c : {14.0, 18.0, 24.0}) {
                CHECK(ev->resourceForDistance(d, c) == energyForDistance(d, c));
                CHECK(ev->distanceOnResource(d, c) == rangeFromEnergy(d, c));
            }
        }
        CHECK(ev->serviceDuration(42.0, 50.0) == chargeDuration(42.0, 50.0));
    }

    SECTION("degenerate inputs keep their existing, defensive meaning") {
        // Zero consumption gives zero range rather than infinite range: a
        // malformed demand strands instead of teleporting.
        CHECK(ev->distanceOnResource(50.0, 0.0) == 0.0);
        // A station with no power serves instantly; the loader rejects these
        // anyway, so this only fixes what the function has always returned.
        CHECK(ev->serviceDuration(50.0, 0.0) == 0.0);
    }

    SECTION("one shared instance") {
        CHECK(electricVehicle().get() == ev.get());
        CHECK(ev->name() == "ev");
    }
}
