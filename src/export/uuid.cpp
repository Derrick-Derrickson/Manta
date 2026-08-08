#include "export/uuid.h"

#include <algorithm>

#include "base/sha1.h"

namespace manta {

std::string formatUuid(const UuidBytes& uuid) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < uuid.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += kHex[uuid[i] >> 4];
        out += kHex[uuid[i] & 0x0f];
    }
    return out;
}

UuidBytes uuidV5(const UuidBytes& ns, std::string_view name) {
    // RFC 4122 4.3: hash the namespace's 16 network-order bytes followed by the
    // name, then take the leading 16 bytes of the digest.
    std::string message;
    message.reserve(ns.size() + name.size());
    message.append(reinterpret_cast<const char*>(ns.data()), ns.size());
    message.append(name);

    Sha1Digest digest = sha1(message);

    UuidBytes out{};
    std::copy_n(digest.begin(), out.size(), out.begin());
    out[6] = static_cast<std::uint8_t>((out[6] & 0x0f) | 0x50);  // version 5
    out[8] = static_cast<std::uint8_t>((out[8] & 0x3f) | 0x80);  // RFC 4122 variant
    return out;
}

std::string pathUuid(const std::vector<std::string>& path, std::size_t count) {
    std::string name;
    for (std::size_t i = 0; i < count && i < path.size(); ++i) {
        if (i) name += '/';
        name += path[i];
    }
    return formatUuid(uuidV5(kMantaNamespace, name));
}

}  // namespace manta
