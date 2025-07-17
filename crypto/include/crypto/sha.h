/*************************************************************************
    > File Name: sha.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月04日 星期五 15时02分54秒
 ************************************************************************/

#ifndef __CRYPTO_SHA_H__
#define __CRYPTO_SHA_H__

#include <stdint.h>
#include <string>
#include <vector>
#include <memory>

#ifndef SHA_DIGEST_LENGTH
#define SHA_DIGEST_LENGTH       20 // SHA-1
#endif

#ifndef SHA256_DIGEST_LENGTH
#define SHA256_DIGEST_LENGTH    32 // SHA-256
#endif

#ifndef SHA512_DIGEST_LENGTH
#define SHA512_DIGEST_LENGTH    64 // SHA-512
#endif

namespace eular {
namespace crypto {
class SHAContext;
class SHA {
public:
    enum {
        SHA_1 = 1,
        SHA_256,
        SHA_512
    };

    SHA() = default;
    ~SHA() = default;
    SHA(const SHA &) = delete;
    SHA &operator=(const SHA &) = delete;

    static std::string Hash(int32_t type, const void *data, int32_t bytes);
    template <typename T>
    static std::string Hash(int32_t type, const std::basic_string<T> &data) {
        if (data.empty()) {
            return std::string();
        }
        return Hash(type, data.data(), static_cast<int32_t>(data.size() * sizeof(T)));
    }
    template <typename T>
    static std::string Hash(int32_t type, const std::vector<T> &data) {
        if (data.empty()) {
            return std::string();
        }
        return Hash(type, data.data(), static_cast<int32_t>(data.size() * sizeof(T)));
    }

    // Initialize the SHA context
    int32_t init(int32_t type);

    // Update the SHA context with new data
    int32_t update(const void *data, int32_t length);
    int32_t update(const std::string &data);
    template <typename T>
    int32_t update(const std::vector<T> &data) {
        if (data.empty()) {
            return 0; // No data to update
        }

        return update(data.data(), static_cast<int32_t>(data.size() * sizeof(T)));
    }

    // Finalize the SHA computation and return the hash
    int32_t finalize(void *hash, int32_t length);
    int32_t finalize(std::string &hash);
    int32_t finalize(std::vector<uint8_t> &hash) {
        int32_t bytes = hashSize();
        hash.resize(bytes);
        int32_t result = finalize(hash.data(), bytes);
        return result;
    }

    // Get the size of the hash output
    int32_t hashSize() const;

private:
    std::unique_ptr<SHAContext>     m_context{};
};

} // namespace crypto
} // namespace eular
#endif // __CRYPTO_SHA_H__
