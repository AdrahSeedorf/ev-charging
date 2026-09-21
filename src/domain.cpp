#include "evnet/domain.hpp"

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

std::shared_ptr<const Domain> electricVehicle() {
    static const std::shared_ptr<const Domain> instance = std::make_shared<ElectricVehicle>();
    return instance;
}

}  // namespace evnet
