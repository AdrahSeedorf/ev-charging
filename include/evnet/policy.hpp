#pragma once

#include <memory>
#include <string>
#include <vector>

#include "evnet/network.hpp"
#include "evnet/units.hpp"

namespace evnet {

/// One evaluated option: "stop at this station, take this much energy".
///
/// The allocator builds these; a Policy only has to rank them. Keeping scoring
/// separate from candidate generation is what allows four quite different
/// strategies to share one feasibility model -- so a comparison between them
/// measures the strategy and nothing else.
struct Candidate {
    NodeId node{kNoNode};
    Km detourKm{0.0};       ///< distance from current position to this station
    Km progressKm{0.0};     ///< reduction in remaining distance to destination
    Kwh energyKwh{0.0};     ///< energy to be taken on here
    /// State of charge on departure. Carried explicitly rather than recomputed by
    /// the caller as (soc - travel + charge): that round trip through subtraction
    /// and re-addition loses the low bits, which was enough to leave a vehicle
    /// needing exactly 70.3 kWh departing with 70.29999999999999 and then failing
    /// a later "can I finish" test by a rounding error.
    Kwh socAfterCharge{0.0};
    Dollars travelCost{0.0};
    Dollars energyCost{0.0};
    Hours waitHours{0.0};
    Hours chargeHours{0.0};

    Dollars moneyCost() const { return travelCost + energyCost; }
    Hours timeHours() const { return waitHours + chargeHours; }

    /// Money and time on one axis. This is the objective neither legacy project
    /// could express: the Sydney study optimised `moneyCost` alone, the corridor
    /// study optimised `waitHours` alone, and the interesting question -- is a
    /// cheaper charger worth a longer queue -- lives in between.
    Dollars generalisedCost(Dollars valueOfTimePerHour) const {
        return moneyCost() + timeHours() * valueOfTimePerHour;
    }
};

/// Strategy for choosing among feasible charging stops.
///
/// Both legacy algorithms survive here as implementations rather than one
/// superseding the other, which is the point: the toolkit's headline output is a
/// comparison table across policies, and each legacy project supplies one row.
class Policy {
public:
    virtual ~Policy() = default;
    virtual std::string name() const = 0;

    /// Returns nullptr only when `candidates` is empty.
    virtual const Candidate* choose(const std::vector<Candidate>& candidates) const = 0;
};

/// Minimise travel + energy cost, ignoring queues.
/// The legacy Sydney metro algorithm.
class CheapestEnergyPolicy : public Policy {
public:
    std::string name() const override { return "cheapest"; }
    const Candidate* choose(const std::vector<Candidate>& candidates) const override;
};

/// Minimise expected queueing time, ignoring price.
/// The legacy Hume corridor algorithm.
class MinWaitPolicy : public Policy {
public:
    std::string name() const override { return "min-wait"; }
    const Candidate* choose(const std::vector<Candidate>& candidates) const override;
};

/// Always push as far down the route as range allows.
/// The naive baseline -- and notably this algorithm was already written in the
/// legacy corridor project, sitting commented out beneath the one that shipped.
/// Restoring it as a first-class policy gives the comparison a control.
class FarthestReachablePolicy : public Policy {
public:
    std::string name() const override { return "farthest"; }
    const Candidate* choose(const std::vector<Candidate>& candidates) const override;
};

/// Minimise money plus time-valued-at-a-rate. The merged objective.
class GeneralisedCostPolicy : public Policy {
public:
    explicit GeneralisedCostPolicy(Dollars valueOfTimePerHour)
        : valueOfTime_(valueOfTimePerHour) {}
    std::string name() const override { return "generalised"; }
    const Candidate* choose(const std::vector<Candidate>& candidates) const override;

private:
    Dollars valueOfTime_;
};

/// Factory for CLI use. Throws std::invalid_argument on an unknown name.
std::unique_ptr<Policy> makePolicy(const std::string& name, Dollars valueOfTimePerHour);

/// Names accepted by makePolicy, in a sensible presentation order.
std::vector<std::string> policyNames();

}  // namespace evnet
