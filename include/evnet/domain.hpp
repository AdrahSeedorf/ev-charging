#pragma once

#include <memory>
#include <string>
#include <vector>

#include "evnet/quantities.hpp"

// The seam, as an interface.
//
// Everything the engine needs to know about *what kind of agent* it is routing
// comes through here. Router, planners, both congestion engines and Siting ask
// a Domain how much resource a leg costs, how far a given amount will carry an
// agent, and how long a station takes to serve it -- and never answer those
// questions themselves.
//
// It began as three methods, because an audit of the engine found exactly three
// physics questions being asked above the seam, with the rule that anything a
// second domain needed beyond them was a finding about the model and belonged
// here rather than in an `if` somewhere in the planner.
//
// Trucking produced two such findings. admission(): the EV model assumed,
// without ever saying so, that a full station has somewhere to wait. Truck
// parking does not, and that difference -- not how fast a clock drains -- is
// what makes a parking shortage cost drivers their legal hours. And
// levelAfterService(): the EV model assumed an agent chooses how much service to
// take. A driver's reset is all or nothing.

namespace evnet {

/// What a station does with an arrival when every server is busy.
///
/// This is a property of the domain rather than of individual stations because
/// it describes what the facility physically is. An EV charging site has a car
/// park: you wait for the next charger. A truck stop's parking IS the server --
/// when every bay is taken there is nowhere to wait, and the driver has to go
/// and look somewhere else.
enum class Admission {
    Queue,     ///< wait for the next free server
    TurnAway,  ///< no waiting room: a full station refuses the arrival
};

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

    /// What a full station does with an arrival. See Admission.
    virtual Admission admission() const = 0;

    /// The level an agent leaves a station at, having arrived at
    /// `levelOnArrival` and asked to be topped up to `requestedLevel`.
    ///
    /// For an EV this is simply what was asked for: you choose how much to
    /// charge. Not every domain works that way -- a mandated rest resets a
    /// driver's clock in full however little of it was used -- so the engine
    /// asks, rather than assuming the request is what gets delivered.
    ///
    /// A level rather than an amount on purpose. The engine carries the level
    /// after service explicitly instead of rebuilding it as arrival + amount,
    /// because that round trip loses low bits: it once left a vehicle needing
    /// exactly 70.3 kWh departing with 70.29999999999999, which then failed a
    /// later can-I-finish check by a rounding error.
    virtual Resource levelAfterService(Resource levelOnArrival, Resource requestedLevel,
                                       Resource capacity) const = 0;
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
    /// You wait for a charger.
    Admission admission() const override { return Admission::Queue; }
    /// You charge to the level you chose.
    Resource levelAfterService(Resource /*levelOnArrival*/, Resource requestedLevel,
                               Resource /*capacity*/) const override {
        return requestedLevel;
    }
};

/// A shared, immutable EV domain. Domains are stateless, so one instance serves
/// every network that uses it.
std::shared_ptr<const Domain> electricVehicle();

/// A solo heavy-vehicle driver under the Heavy Vehicle National Law's standard
/// hours, which apply on the Hume in both NSW and Victoria: no more than 12
/// hours' work in any 24 hours, with at least 7 continuous hours of stationary
/// rest (NHVR, "Work and rest requirements").
///
///   resource   work hours left before a major rest is required; at most 12
///   drains     while driving, one hour of work per hour at the wheel
///   service    a 7-hour continuous rest, which restores all 12 hours however
///              few were used -- the rest is mandated, not chosen
///   admission  a full rest area turns the truck away: the bays ARE the servers
///
/// Deliberate simplifications, each stated so it can be argued with:
///
///   * The short breaks -- 15 minutes by 5.5 hours of work, 30 by 8, 60 by 11 --
///     are not modelled. They would add a second and third clock that the
///     engine's single resource cannot carry, and they also use rest-area bays,
///     so this domain UNDERSTATES demand for parking. A shortage it finds is
///     a lower bound.
///   * Only driving counts as work. Loading, fuelling and paperwork also count
///     under the law; a trip's starting level can account for work done before
///     it enters the network.
///
/// Consumption: for a driver, "resource per km" is hours per km, which is a
/// statement about SPEED -- and the simulator already has a speed. Were the two
/// allowed to differ, a driver's clock would run at one speed while the truck
/// drove at another. So this domain is built with the simulator's speed, and an
/// agent's `consumption` is the fraction of its driving time that counts as
/// work: 1 for any real driver.
class HeavyVehicleStandardHours final : public Domain {
public:
    static constexpr Hours kMaxWorkHours = 12.0;
    static constexpr Hours kMajorRestHours = 7.0;

    explicit HeavyVehicleStandardHours(double speedKmh);

    std::string name() const override { return "truck"; }
    Resource resourceForDistance(Km distance, PerDistance consumption) const override;
    Km distanceOnResource(Resource amount, PerDistance consumption) const override;
    /// The rest takes seven hours whatever it restores; `rate` is meaningless.
    Hours serviceDuration(Resource amount, Rate rate) const override;
    /// A full rest area has nowhere to wait.
    Admission admission() const override { return Admission::TurnAway; }
    /// Rest at all and the clock comes back in full.
    Resource levelAfterService(Resource levelOnArrival, Resource requestedLevel,
                               Resource capacity) const override;

    double speedKmh() const { return speedKmh_; }

private:
    double speedKmh_;
};

/// The domain a dataset names, e.g. in its domain.txt. `speedKmh` must be the
/// simulator's: a domain whose resource is time needs it (see
/// HeavyVehicleStandardHours), and the EV domain ignores it. Throws
/// std::invalid_argument, listing the known names, for anything else.
std::shared_ptr<const Domain> makeDomain(const std::string& name, double speedKmh);

/// Names makeDomain accepts.
std::vector<std::string> domainNames();

}  // namespace evnet
