#ifndef ROV_INTERNAL_ROV_CONTROL_TEST_ACCESS_HPP
#define ROV_INTERNAL_ROV_CONTROL_TEST_ACCESS_HPP

#include "rov/rov_control.hpp"

#include <memory>

namespace rov::internal {

class RovControlTestAccess final {
public:
    static std::unique_ptr<RovControl> create(
        std::unique_ptr<ITransport> transport)
    {
        return std::unique_ptr<RovControl>(
            new RovControl(std::move(transport)));
    }
};

} // namespace rov::internal

#endif // ROV_INTERNAL_ROV_CONTROL_TEST_ACCESS_HPP
