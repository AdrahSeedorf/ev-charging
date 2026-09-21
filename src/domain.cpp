#include "evnet/domain.hpp"

#include <stdexcept>

#include "evnet/units.hpp"

namespace evnet {

// The EV domain delegates to units.hpp rather than restating the formulas, so
// there is one copy of the physics and this file is only the adapter that puts
// it behind the seam. This is the only translation unit above the seam's
// consumers that includes units.hpp.

Resource ElectricVehicle::resourceForDistance(Km distance, PerDistance consumption) const {
    return energyForDistance(distance, consumption);
}

Km ElectricVehicle::distanceOnResource(Resource amount, PerDistance consumption) const {
    return rangeFromEnergy(amount, consumption);
}

Hours ElectricVehicle::serviceDuration(Resource amount, Rate rate) const {
    return chargeDuration(amount, rate);
}

HeavyVehicleStandardHours::HeavyVehicleStandardHours(double speedKmh) : speedKmh_(speedKmh) {
    if (!(speedKmh > 0.0)) {
        throw std::invalid_argument("truck domain: speed must be positive");
    }
}

Resource HeavyVehicleStandardHours::resourceForDistance(Km distance, PerDistance consumption) const {
    return drivingTime(distance, speedKmh_) * consumption;
}

Km HeavyVehicleStandardHours::distanceOnResource(Resource amount, PerDistance consumption) const {
    // Same defensive meaning as the EV domain: no consumption, no range.
    if (consumption <= 0.0) return 0.0;
    return amount / consumption * speedKmh_;
}

Hours HeavyVehicleStandardHours::serviceDuration(Resource amount, Rate /*rate*/) const {
    return amount > 0.0 ? kMajorRestHours : 0.0;
}

Resource HeavyVehicleStandardHours::levelAfterService(Resource levelOnArrival, Resource requestedLevel,
                                                      Resource capacity) const {
    return requestedLevel > levelOnArrival ? capacity : levelOnArrival;
}

std::shared_ptr<const Domain> electricVehicle() {
    static const std::shared_ptr<const Domain> instance = std::make_shared<ElectricVehicle>();
    return instance;
}

std::vector<std::string> domainNames() { return {"ev", "truck"}; }

std::shared_ptr<const Domain> makeDomain(const std::string& name, double speedKmh) {
    if (name == "ev") return electricVehicle();
    if (name == "truck") return std::make_shared<HeavyVehicleStandardHours>(speedKmh);
    std::string known;
    for (const auto& n : domainNames()) known += (known.empty() ? "" : ", ") + n;
    throw std::invalid_argument("unknown domain '" + name + "' (expected one of: " + known + ")");
}

}  // namespace evnet
