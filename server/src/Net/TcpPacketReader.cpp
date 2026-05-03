#include "TcpPacketReader.hpp"

#include "TcpPacket.hpp"

namespace Net {
bool TcpPacketReader::appendBytes(const uint8_t* data, size_t size) {
    if (size == 0) {
        return true;
    }

    if (data == nullptr) {
        return false;
    }

    buffer_.insert(buffer_.end(), data, data + size);
    return true;
}

TcpPacketReadResult TcpPacketReader::tryReadPacket(std::vector<uint8_t>& outPacket) {
    if (buffer_.size() < kTcpHeaderSize) {
        return TcpPacketReadResult::kNeedMoreData;
    }

    TcpPacketHeader header;
    if (!peekTcpPacketHeader(buffer_.data(), buffer_.size(), header)) {
        return TcpPacketReadResult::kInvalidPacket;
    }

    if (header.size < kTcpHeaderSize || header.size > kMaxTcpPacketSize) {
        return TcpPacketReadResult::kInvalidPacket;
    }

    if (buffer_.size() < header.size) {
        return TcpPacketReadResult::kNeedMoreData;
    }

    outPacket.assign(buffer_.begin(), buffer_.begin() + header.size);
    buffer_.erase(buffer_.begin(), buffer_.begin() + header.size);
    return TcpPacketReadResult::kPacketReady;
}

size_t TcpPacketReader::bufferedSize() const {
    return buffer_.size();
}

void TcpPacketReader::reset() {
    buffer_.clear();
}
}  // namespace Net
