#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Net {
enum class TcpPacketReadResult {
    kNeedMoreData,
    kPacketReady,
    kInvalidPacket,
};

class TcpPacketReader {
public:
    bool appendBytes(const uint8_t* data, size_t size);
    TcpPacketReadResult tryReadPacket(std::vector<uint8_t>& outPacket);

    size_t bufferedSize() const;
    void reset();

private:
    std::vector<uint8_t> buffer_;
};
}  // namespace Net
