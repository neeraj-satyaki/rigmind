// Password hashing for the local RigMind accounts: PBKDF2-HMAC-SHA256 (RFC 8018) with a random 16-byte salt per account.
// Portable (no OS crypto library), so the Windows and Linux hosts verify the same stored hashes. The only OS call is the
// random source (BCryptGenRandom on Windows, /dev/urandom elsewhere).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rigmind {

using Bytes = std::vector<std::uint8_t>;

Bytes sha256(const Bytes& data);
Bytes hmac_sha256(const Bytes& key, const Bytes& data);
// dk_len bytes of PBKDF2-HMAC-SHA256(password, salt, iterations).
Bytes pbkdf2_sha256(const std::string& password, const Bytes& salt, std::uint32_t iterations, std::size_t dk_len);

Bytes random_bytes(std::size_t count); // throws std::runtime_error when the system source fails
std::string to_hex(const Bytes& bytes);
Bytes from_hex(const std::string& hex); // throws std::invalid_argument on odd length or bad digits
bool constant_time_equal(const Bytes& a, const Bytes& b);

} // namespace rigmind
