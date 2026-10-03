// SPDX-License-Identifier: GPL-3.0-or-later

#include "sha256.h"

#include <openssl/sha.h>

namespace grpc_poppler {

std::string Sha256Hex(const std::string& data) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(SHA256_DIGEST_LENGTH * 2);
  for (unsigned char b : digest) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

}  // namespace grpc_poppler
