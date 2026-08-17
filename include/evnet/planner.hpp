#pragma once

#include <memory>
#include <string>
#include <vector>

#include "evnet/candidates.hpp"
#include "evnet/network.hpp"
#include "evnet/policy.hpp"
#include "evnet/router.hpp"
#include "evnet/units.hpp"
#include "evnet/wait_oracle.hpp"

namespace evnet {

/// What a vehicle should do next.
struct Action {
    enum class Kind {
        DriveToDestination,  ///< enough charge to finish
        DriveToStation,      ///< head for `target`, decide again on arrival
        ChargeHere,          ///< take on `energyKwh` at the current node
        Infeasible           ///< nothing legal remains; `reason` says why
    };

    Kind kind{Kind::Infeasible};
    NodeId target{kNoNode};
    Kwh energyKwh{0.0};
    std::string reason;

    static Action driveToDestination() { return {Kind::DriveToDestination, kNoNode, 0.0, {}}; }
    static Action driveTo(NodeId node) { return {Kind::DriveToStation, node, 0.0, {}}; }
    static Action chargeHere(Kwh energy) { return {Kind::ChargeHere, kNoNode, energy, {}}; }
    static Action infeasible(std::string why) {
        return {Kind::Infeasible, kNoNode, 0.0, std::move(why)};
    }
};

/// Decides a vehicle's next move.
///
/// Called afresh every time a vehicle reaches a node, so every planner here is
/// receding-horizon: it commits only to the next step and reconsiders on arrival
/// with whatever congestion has actually materialised. That is what lets a vehicle
/// change its mind when the station it was heading for turns out to be busier than
/// estimated.
class Planner {
public:
    virtual ~Planner() = default;
    virtual std::string name() const = 0;
    virtual Action decide(const VehicleState& vehicle, const WaitOracle& oracle) const = 0;

    /// How this planner ranks a set of already-enumerated options.
    ///
    /// Used for round-trip top-up missions, which contain exactly ONE decision --
    /// pick a station, charge, come home. With a single decision there is no
    /// lookahead to be myopic about, so scoring candidates directly is optimal by
    /// construction and the optimal planner needs no special case.
    virtual const Policy& scoringPolicy() const = 0;
};

/// Wraps a stage-1 Policy. Scores only the immediate stop, which is exactly the
/// myopia the optimal planner exists to remove: on the Hume corridor this makes
/// the `cheapest` policy take eight short hops to save about two dollars.
class GreedyPlanner : public Planner {
public:
    GreedyPlanner(const Network& network,
                  const Router& router,
                  std::unique_ptr<Policy> policy,
                  FeasibilityConfig config = {});

    std::string name() const override { return policy_->name(); }
    Action decide(const VehicleState& vehicle, const WaitOracle& oracle) const override;
    const Policy& scoringPolicy() const override { return *policy_; }

private:
    const Network* network_;
    const Router* router_;
    std::unique_ptr<Policy> policy_;
    FeasibilityConfig config_;
};

/// Minimises generalised cost over the whole remaining journey.
///
/// Runs Dijkstra over a state space of (node, discretised charge) rather than over
/// nodes alone, which is what makes charging decisions and routing decisions
/// jointly optimal instead of independently greedy. Transitions are:
///
///   * drive along one edge, spending energy and time, if the reserve survives;
///   * at a station, raise the charge level, paying for energy plus the estimated
///     wait and the transfer time.
///
/// State count is nodes x (levels + 1) -- 984 for the Sydney network at the default
/// 40 levels -- so this is cheap despite sounding expensive. Resolution matters:
/// each level is a chunk of energy the planner must buy in whole units, so a coarse
/// grid quietly taxes every charging stop.
///
/// HONEST LIMIT: optimal with respect to congestion *as currently estimated*, not
/// clairvoyant. A station's wait is taken from the oracle at planning time and
/// treated as fixed for the duration of the plan; arrival times are approximated
/// from shortest-path distance at the configured speed. Since the plan is
/// recomputed at every stop, estimation error corrects itself rather than
/// accumulating. Genuine optimality would need congestion to be part of the state,
/// which it cannot be while other vehicles are still choosing.
class OptimalPlanner : public Planner {
public:
    OptimalPlanner(const Network& network,
                   const Router& router,
                   Dollars valueOfTimePerHour,
                   FeasibilityConfig config = {},
                   int chargeLevels = 20);

    std::string name() const override { return "optimal"; }
    Action decide(const VehicleState& vehicle, const WaitOracle& oracle) const override;
    const Policy& scoringPolicy() const override { return scoring_; }

    /// Total generalised cost of the best plan from this state, or infinity if no
    /// plan exists. Exposed for tests and for reporting the optimality gap.
    Dollars planCost(const VehicleState& vehicle, const WaitOracle& oracle) const;

private:
    struct Plan {
        Dollars cost{0.0};
        Action first;
        bool feasible{false};
    };
    Plan solve(const VehicleState& vehicle, const WaitOracle& oracle) const;

    const Network* network_;
    const Router* router_;
    Dollars valueOfTime_;
    FeasibilityConfig config_;
    int levels_;
    GeneralisedCostPolicy scoring_;
};

/// Builds a planner by name: the four stage-1 policies plus "optimal".
std::unique_ptr<Planner> makePlanner(const std::string& name,
                                     const Network& network,
                                     const Router& router,
                                     Dollars valueOfTimePerHour,
                                     FeasibilityConfig config = {});

/// Planner names in presentation order, naive first.
std::vector<std::string> plannerNames();

}  // namespace evnet
