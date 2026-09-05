#include "internal/transport.hpp"

namespace rov::internal {

WriteResult ITransport::writeAll(std::string_view data)
{
    std::size_t offset = 0;

    while (offset < data.size()) {
        WriteResult result = writeSome(data.data() + offset,
                                       data.size() - offset);
        if (result.status == IoStatus::Retry) {
            continue;
        }
        if (result.status != IoStatus::Ok) {
            result.bytesTransferred += offset;
            return result;
        }
        if (result.bytesTransferred == 0 ||
            result.bytesTransferred > (data.size() - offset)) {
            return {IoStatus::Error, offset, 0,
                    "transport returned an invalid short-write count"};
        }
        offset += result.bytesTransferred;
    }

    return {IoStatus::Ok, offset, 0, {}};
}

} // namespace rov::internal
