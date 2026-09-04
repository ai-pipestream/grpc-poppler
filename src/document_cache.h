#pragma once

#include <cstddef>
#include <cstdlib>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace grpc_poppler {

// In-memory content-addressed document cache backing the PdfDocument.sha256
// handshake: clients upload bytes once and address them by lowercase hex
// SHA-256 on later calls. Shared across requests, so every entry point is
// mutex-guarded. Bounds are an LRU document count plus a total-bytes
// ceiling; both come from the environment
// (GRPC_POPPLER_CACHE_MAX_DOCUMENTS, GRPC_POPPLER_CACHE_MAX_BYTES) unless
// explicit limits are passed, which is what the tests do.
class DocumentCache {
 public:
  struct Limits {
    size_t max_documents = 8;
    size_t max_bytes = 2ull * 1024 * 1024 * 1024;
  };

  // Reads GRPC_POPPLER_CACHE_MAX_DOCUMENTS (default 8) and
  // GRPC_POPPLER_CACHE_MAX_BYTES (default 2 GiB); 0 disables caching.
  static Limits LimitsFromEnv() {
    Limits limits;
    if (const char* env = std::getenv("GRPC_POPPLER_CACHE_MAX_DOCUMENTS")) {
      limits.max_documents = std::strtoull(env, nullptr, 10);
    }
    if (const char* env = std::getenv("GRPC_POPPLER_CACHE_MAX_BYTES")) {
      limits.max_bytes = std::strtoull(env, nullptr, 10);
    }
    return limits;
  }

  explicit DocumentCache(Limits limits) : limits_(limits) {}

  // The cached bytes for a hash, or nullptr on a miss. Hits refresh the
  // entry's LRU position.
  std::shared_ptr<const std::string> Lookup(const std::string& sha256_hex) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(sha256_hex);
    if (it == entries_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.lru_it);
    return it->second.bytes;
  }

  // Caches bytes under their verified hash. A document larger than the byte
  // ceiling (or a disabled cache) is simply never stored.
  void Insert(const std::string& sha256_hex, const std::string& bytes) {
    if (limits_.max_documents == 0 || bytes.size() > limits_.max_bytes) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto existing = entries_.find(sha256_hex);
    if (existing != entries_.end()) {
      lru_.splice(lru_.begin(), lru_, existing->second.lru_it);
      return;
    }
    lru_.push_front(sha256_hex);
    entries_.emplace(sha256_hex,
                     Entry{std::make_shared<const std::string>(bytes),
                           lru_.begin()});
    total_bytes_ += bytes.size();
    while (entries_.size() > limits_.max_documents ||
           total_bytes_ > limits_.max_bytes) {
      const std::string& oldest = lru_.back();
      total_bytes_ -= entries_.at(oldest).bytes->size();
      entries_.erase(oldest);
      lru_.pop_back();
    }
  }

 private:
  struct Entry {
    std::shared_ptr<const std::string> bytes;
    std::list<std::string>::iterator lru_it;
  };

  const Limits limits_;
  std::mutex mutex_;
  // Front is most recently used.
  std::list<std::string> lru_;
  std::unordered_map<std::string, Entry> entries_;
  size_t total_bytes_ = 0;
};

}  // namespace grpc_poppler
