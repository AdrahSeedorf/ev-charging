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

// ---------------------------------------------------------------------------
// Does every physics question actually go through the domain?
//
// Byte-identical EV output proves the refactor broke nothing, but it cannot
// prove the seam is real: a call site that still did its own EV arithmetic
// would give the same EV answer. This test is built to catch exactly that.
//
// Scaled(k) is a domain in which every leg costs k times what EV says, every
// amount carries 1/k as far, and every service takes as long as it would at
// 1/k of the charger's rate. Running it on the Hume data must give the SAME
// answer as running the plain EV domain on Hume data rescaled to match --
// every vehicle's consumption times k, every charger's power divided by k.
// k is 2 or 1/2, and scaling by a power of two is exact in binary floating
// point, so "the same" means bit-identical.
//
// Any call site that bypasses the domain sees the unscaled data in one run and
// the scaled data in the other, and the two runs diverge.
// ---------------------------------------------------------------------------

#include "evnet/candidates.hpp"
#include "evnet/planner.hpp"
#include "evnet/simulation.hpp"
#include "evnet/simulator.hpp"
#include "evnet/siting.hpp"
#include "evnet/station_runtime.hpp"
#include "evnet/station_state.hpp"

#include <limits>
#include <string>
#include <vector>

namespace {

/// EV physics with every cost scaled by k: legs cost k times as much, an amount
/// carries 1/k as far, and service takes as long as it would at 1/k of the
/// charger's rate. k is a power of two, so every rescaling is exact.
class Scaled final : public Domain {
public:
    explicit Scaled(double k) : k_(k) {}
    std::string name() const override { return "scaled"; }
    Resource resourceForDistance(Km d, PerDistance c) const override {
        return k_ * ev_->resourceForDistance(d, c);
    }
    Km distanceOnResource(Resource r, PerDistance c) const override {
        return ev_->distanceOnResource(r, c) / k_;
    }
    Hours serviceDuration(Resource a, Rate r) const override { return ev_->serviceDuration(a, r / k_); }
    Admission admission() const override { return ev_->admission(); }

private:
    double k_;
    std::shared_ptr<const Domain> ev_ = electricVehicle();
};

struct Scenario {
    Network network;
    std::vector<Demand> demands;
};

Scenario load(const std::string& dataset) {
    const std::string root = std::string(EVNET_PROJECT_ROOT) + "/data/" + dataset + "/";
    return {Network::load(root + "nodes.csv", root + "edges.csv"), Demand::load(root + "demands.csv")};
}

Scenario hume() { return load("hume"); }

/// The scaled domain on the original data.
Scenario viaDomain(double k, const std::string& dataset = "hume") {
    Scenario s = load(dataset);
    s.network.setDomain(std::make_shared<Scaled>(k));
    return s;
}

/// The EV domain on data rescaled to mean the same thing.
Scenario viaData(double k, const std::string& dataset = "hume") {
    Scenario s = load(dataset);
    for (Demand& d : s.demands) d.consumption *= k;
    for (const NodeId id : s.network.stationNodes()) {
        Station st = *s.network.node(id).station;
        st.ratePerHour /= k;
        s.network.setStation(id, st);
    }
    return s;
}

void requireSameStops(const std::vector<Stop>& a, const std::vector<Stop>& b) {
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].node == b[i].node);
        CHECK(a[i].amount == b[i].amount);
        CHECK(a[i].waitHours == b[i].waitHours);
        CHECK(a[i].serviceHours == b[i].serviceHours);
    }
}

std::vector<TimedTrip> simulate(const Scenario& s, const std::string& planner) {
    const Router router(s.network);
    const SimulatorConfig config;
    const auto p = makePlanner(planner, s.network, router, config.valueOfTimePerHour, config.feasibility());
    const Simulator simulator(s.network, router, config);
    StationRuntime runtime(s.network, config.stopOverheadHours);
    return simulator.run(s.demands, *p, runtime);
}

std::vector<TripResult> allocate(const Scenario& s, const std::string& policy) {
    const Router router(s.network);
    const SimulationConfig config;
    const Allocator allocator(s.network, router, config);
    const auto p = makePolicy(policy, config.valueOfTimePerHour);
    StationState state(s.network);
    return allocator.run(s.demands, *p, state);
}

}  // namespace

// Both directions matter. Doubling can only make a bypassed range calculation
// OVERestimate range, and the arrival check then quietly rejects the extra
// stations it lets in -- so a bypass in the range bound survives k = 2 and is
// only exposed at k = 0.5, where it wrongly prunes stations that were reachable.
TEST_CASE("every physics question in the event-driven engine goes through the domain", "[domain][seam]") {
  // Sydney as well as Hume: only Sydney has top-up missions, and they take a
  // separate path through the candidate builder.
  for (const std::string dataset : {"hume", "sydney"}) {
  for (const double k : {2.0, 0.5}) {
    INFO(dataset << ", scale " << k);
    const Scenario a = viaDomain(k, dataset);
    const Scenario b = viaData(k, dataset);
    for (const std::string& planner : plannerNames()) {
        INFO("planner " << planner);
        const auto ta = simulate(a, planner);
        const auto tb = simulate(b, planner);
        REQUIRE(ta.size() == tb.size());
        for (std::size_t i = 0; i < ta.size(); ++i) {
            INFO("vehicle " << ta[i].demandId);
            CHECK(ta[i].completed == tb[i].completed);
            CHECK(ta[i].failure == tb[i].failure);
            CHECK(ta[i].finishTime == tb[i].finishTime);
            CHECK(ta[i].distanceKm == tb[i].distanceKm);
            CHECK(ta[i].energyCost == tb[i].energyCost);
            CHECK(ta[i].levelAtFinish == tb[i].levelAtFinish);
            if (ta[i].completed) CHECK(ta[i].levelAtFinish >= 0.0);
            requireSameStops(ta[i].stops, tb[i].stops);
        }
    }
  }
  }
}

TEST_CASE("every physics question in the static engine goes through the domain", "[domain][seam]") {
  // Sydney as well as Hume: only Sydney has top-up missions, and they take a
  // separate path through the candidate builder.
  for (const std::string dataset : {"hume", "sydney"}) {
  for (const double k : {2.0, 0.5}) {
    INFO(dataset << ", scale " << k);
    const Scenario a = viaDomain(k, dataset);
    const Scenario b = viaData(k, dataset);
    for (const std::string& policy : policyNames()) {
        INFO("policy " << policy);
        const auto ra = allocate(a, policy);
        const auto rb = allocate(b, policy);
        REQUIRE(ra.size() == rb.size());
        for (std::size_t i = 0; i < ra.size(); ++i) {
            INFO("vehicle " << ra[i].demandId);
            CHECK(ra[i].completed == rb[i].completed);
            CHECK(ra[i].distanceKm == rb[i].distanceKm);
            CHECK(ra[i].energyCost == rb[i].energyCost);
            CHECK(ra[i].waitHours == rb[i].waitHours);
            CHECK(ra[i].serviceHours == rb[i].serviceHours);
            requireSameStops(ra[i].stops, rb[i].stops);
        }
    }
  }
  }
}

TEST_CASE("candidate feasibility asks the domain, including where its guards bind", "[domain][seam]") {
    // Comparing trips is not enough for the candidate builder. Its onward
    // feasibility guard is lenient -- a station is kept if ANY onward station in
    // range makes progress -- and with the shipped fleets' battery sizes there
    // almost always is one, so a wrong range bound rarely changes a decision.
    // Compare the candidate lists themselves, over a sweep of battery sizes and
    // charge levels that includes batteries small enough for the guards to bind.
    const SimulatorConfig config;
    for (const std::string dataset : {"hume", "sydney"}) {
        for (const double k : {2.0, 0.5}) {
            INFO(dataset << ", scale " << k);
            const Scenario a = viaDomain(k, dataset);
            const Scenario b = viaData(k, dataset);
            const Router ra(a.network), rb(b.network);
            const StationRuntime wa(a.network, config.stopOverheadHours), wb(b.network, config.stopOverheadHours);
            std::size_t compared = 0;
            for (std::size_t i = 0; i < a.demands.size(); ++i) {
                const Demand& d = a.demands[i];
                const PerDistance cb = b.demands[i].consumption;
                for (const Resource capacity : {15.0, 30.0, 60.0}) {
                    for (const double fraction : {0.2, 0.5, 0.9}) {
                        const Resource level = capacity * fraction;
                        const AgentState sa{d.id, d.origin, d.destination, level, capacity, d.consumption, 0.0};
                        const AgentState sb{d.id, d.origin, d.destination, level, capacity, cb, 0.0};
                        const auto ca = d.isTopUp()
                            ? buildTopUpCandidates(a.network, ra, wa, sa, d.requiredAmount, config.feasibility())
                            : buildCandidates(a.network, ra, wa, sa, config.feasibility());
                        const auto cbs = d.isTopUp()
                            ? buildTopUpCandidates(b.network, rb, wb, sb, d.requiredAmount, config.feasibility())
                            : buildCandidates(b.network, rb, wb, sb, config.feasibility());
                        INFO("demand " << d.id << ", capacity " << capacity << ", level " << level);
                        REQUIRE(ca.size() == cbs.size());
                        for (std::size_t j = 0; j < ca.size(); ++j) {
                            CHECK(ca[j].node == cbs[j].node);
                            CHECK(ca[j].amount == cbs[j].amount);
                            CHECK(ca[j].levelAfter == cbs[j].levelAfter);
                            CHECK(ca[j].serviceHours == cbs[j].serviceHours);
                        }
                        compared += ca.size();
                    }
                }
            }
            CHECK(compared > 0);
        }
    }
}

TEST_CASE("the optimal planner prices plans through the domain", "[domain][seam]") {
    // The optimal planner's cost estimate can change without changing any
    // decision on a given dataset -- a cheaper service estimate need not move
    // the argmin -- so its plan cost is compared directly rather than inferred
    // from the trips it produces.
    for (const double k : {2.0, 0.5}) {
        INFO("scale " << k);
        const Scenario a = viaDomain(k);
        const Scenario b = viaData(k);
        const Router ra(a.network), rb(b.network);
        const SimulatorConfig config;
        const auto pa = makePlanner("optimal", a.network, ra, config.valueOfTimePerHour, config.feasibility());
        const auto pb = makePlanner("optimal", b.network, rb, config.valueOfTimePerHour, config.feasibility());
        const auto& oa = dynamic_cast<const OptimalPlanner&>(*pa);
        const auto& ob = dynamic_cast<const OptimalPlanner&>(*pb);
        StationRuntime wa(a.network, config.stopOverheadHours), wb(b.network, config.stopOverheadHours);
        int priced = 0;
        for (std::size_t i = 0; i < a.demands.size(); ++i) {
            const Demand& da = a.demands[i];
            const Demand& db = b.demands[i];
            const AgentState sa{da.id, da.origin, da.destination, da.level, da.capacity, da.consumption, 0.0};
            const AgentState sb{db.id, db.origin, db.destination, db.level, db.capacity, db.consumption, 0.0};
            const Dollars ca = oa.planCost(sa, wa);
            CHECK(ca == ob.planCost(sb, wb));
            if (ca != std::numeric_limits<Dollars>::infinity()) ++priced;
        }
        CHECK(priced > 0);
    }
}

TEST_CASE("siting asks the domain too, under both engines", "[domain][seam]") {
    // Sydney rather than Hume: every Hume town already has a station, so Hume
    // has no candidate sites and siting there would compare two empty lists.
    const Scenario a = viaDomain(2.0, "sydney");
    const Scenario b = viaData(2.0, "sydney");
    const Station prototype{0.45, 4, 150.0};
    Station halved = prototype;  // the prototype, rescaled as viaData rescales every station
    halved.ratePerHour /= 2.0;

    const auto sa = Siting(a.network, SimulatorConfig{}).rankTimed(a.demands, "generalised", prototype, 0);
    const auto sb = Siting(b.network, SimulatorConfig{}).rankTimed(b.demands, "generalised", halved, 0);
    REQUIRE(sa.size() == sb.size());
    REQUIRE_FALSE(sa.empty());
    for (std::size_t i = 0; i < sa.size(); ++i) {
        CHECK(sa[i].node == sb[i].node);
        CHECK(sa[i].meanGeneralisedCost == sb[i].meanGeneralisedCost);
        CHECK(sa[i].adoptionShare == sb[i].adoptionShare);
    }

    const auto policy = makePolicy("generalised", SimulationConfig{}.valueOfTimePerHour);
    const auto xa = Siting(a.network).rank(a.demands, *policy, prototype, 0);
    const auto xb = Siting(b.network).rank(b.demands, *policy, halved, 0);
    REQUIRE(xa.size() == xb.size());
    for (std::size_t i = 0; i < xa.size(); ++i) {
        CHECK(xa[i].node == xb[i].node);
        CHECK(xa[i].meanGeneralisedCost == xb[i].meanGeneralisedCost);
    }
}

TEST_CASE("the seam test is sensitive: the domain changes the answer", "[domain][seam]") {
    // Without this, the three tests above would also pass if the engine ignored
    // consumption and charger power entirely -- both runs would agree because
    // neither used the numbers that differ. Scaling must actually move results.
    const Scenario doubled = viaDomain(2.0);
    const Scenario plain = hume();
    const auto td = simulate(doubled, "generalised");
    const auto tp = simulate(plain, "generalised");
    REQUIRE(td.size() == tp.size());
    int differing = 0;
    for (std::size_t i = 0; i < td.size(); ++i) {
        if (td[i].finishTime != tp[i].finishTime || td[i].stops.size() != tp[i].stops.size()) ++differing;
    }
    CHECK(differing > static_cast<int>(td.size()) / 2);
}
