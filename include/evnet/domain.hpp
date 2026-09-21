#pragma once

#include <memory>
#include <string>

#include "evnet/quantities.hpp"

// The seam, as an interface.
//
// Everything the engine needs to know about *what kind of agent* it is routing
// comes through here. Router, planners, both congestion engines and Siting ask
// a Domain how much resource a leg costs, how far a given amount will carry an
// agent, and how long a station takes to serve it -- and never answer those
// questions themselves.
//
// Three methods, because an audit of the engine found exactly three physics
// questions being asked above the seam. If a second domain needs a fourth, that
// is a finding about the model, and it belongs here rather than in an `if`
// somewhere in the planner.

namespace evnet {

class Domain {
public:
    virtual ~Domain() = default;

    /// Short identifier, e.g. "ev". Used in output and in dataset manifests.
    virtual std::string name() const = 0;

    /// Resource an agent with this consumption spends covering `distance`.
    virtual Resource resourceForDistance(Km distance, PerDistance consumption) const = 0;

    /// How far an agent with this consumption can travel on `amount`. Must
    /// invert resourceForDistance: the engine uses one to decide what is
    /// reachable and the other to charge for getting there, and any gap
    /// between them is a vehicle that plans a leg it then cannot drive.
    virtual Km distanceOnResource(Resource amount, PerDistance consumption) const = 0;

    /// Time in service to take on `amount` at a station of the given rate.
    /// Excludes queueing and any fixed per-stop overhead, which the engines
    /// account for separately.
    virtual Hours serviceDuration(Resource amount, Rate rate) const = 0;
};

/// Charge in kWh, spent at a consumption in kWh/100km, delivered at a charger
/// power in kW. The toolkit's first domain, and the one every shipped dataset
/// is in.
class ElectricVehicle final : public Domain {
public:
    std::string name() const override { return "ev"; }
    Resource resourceForDistance(Km distance, PerDistance consumption) const override;
    Km distanceOnResource(Resource amount, PerDistance consumption) const override;
    Hours serviceDuration(Resource amount, Rate rate) const override;
};

/// A shared, immutable EV domain. Domains are stateless, so one instance serves
/// every network that uses it.
std::shared_ptr<const Domain> electricVehicle();

}  // namespace evnet
