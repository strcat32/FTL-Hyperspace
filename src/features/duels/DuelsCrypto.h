#pragma once

#include <cstddef>
#include <cstdint>

// SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 2104), for the relay's packet tags (docs/design/relay-protocol.md). No
// game headers, so the netcode tests build it outside the game.
namespace Duels
{
    namespace Crypto
    {
        static const size_t SHA256_SIZE = 32;

        class Sha256
        {
        public:
            Sha256();
            void Update(const uint8_t *data, size_t size);
            void Final(uint8_t digest[SHA256_SIZE]);

        private:
            void Block(const uint8_t block[64]);
            uint32_t state[8];
            uint8_t buffer[64];
            size_t used = 0;
            uint64_t length = 0;
        };

        void Sha256Digest(const uint8_t *data, size_t size, uint8_t digest[SHA256_SIZE]);
        void HmacSha256(const uint8_t *key, size_t keySize, const uint8_t *data, size_t size, uint8_t mac[SHA256_SIZE]);

        // Compares without an early exit, so the time taken doesn't tell how many bytes matched.
        bool Equal(const uint8_t *a, const uint8_t *b, size_t size);
    }
}
