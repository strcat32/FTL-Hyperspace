#include "DuelsCrypto.h"

#include <cstring>

namespace Duels
{
    namespace Crypto
    {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        static inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

        Sha256::Sha256()
        {
            static const uint32_t INITIAL[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
            std::memcpy(state, INITIAL, sizeof(state));
        }

        void Sha256::Block(const uint8_t block[64])
        {
            uint32_t w[64];
            for (int i = 0; i < 16; ++i)
            {
                w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 |
                       (uint32_t)block[i * 4 + 3];
            }
            for (int i = 16; i < 64; ++i)
            {
                uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
            uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
            for (int i = 0; i < 64; ++i)
            {
                uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
                uint32_t choice = (e & f) ^ (~e & g);
                uint32_t t1 = h + s1 + choice + K[i] + w[i];
                uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
                uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
                uint32_t t2 = s0 + majority;
                h = g;
                g = f;
                f = e;
                e = d + t1;
                d = c;
                c = b;
                b = a;
                a = t1 + t2;
            }
            state[0] += a;
            state[1] += b;
            state[2] += c;
            state[3] += d;
            state[4] += e;
            state[5] += f;
            state[6] += g;
            state[7] += h;
        }

        void Sha256::Update(const uint8_t *data, size_t size)
        {
            length += size;
            while (size > 0)
            {
                size_t take = 64 - used < size ? 64 - used : size;
                std::memcpy(buffer + used, data, take);
                used += take;
                data += take;
                size -= take;
                if (used == 64)
                {
                    Block(buffer);
                    used = 0;
                }
            }
        }

        void Sha256::Final(uint8_t digest[SHA256_SIZE])
        {
            uint64_t bits = length * 8;
            uint8_t pad = 0x80;
            Update(&pad, 1);
            uint8_t zero = 0;
            while (used != 56) Update(&zero, 1);
            uint8_t size[8];
            for (int i = 0; i < 8; ++i) size[i] = (uint8_t)(bits >> (56 - 8 * i));
            Update(size, 8);
            for (int i = 0; i < 8; ++i)
            {
                digest[i * 4] = (uint8_t)(state[i] >> 24);
                digest[i * 4 + 1] = (uint8_t)(state[i] >> 16);
                digest[i * 4 + 2] = (uint8_t)(state[i] >> 8);
                digest[i * 4 + 3] = (uint8_t)state[i];
            }
        }

        void Sha256Digest(const uint8_t *data, size_t size, uint8_t digest[SHA256_SIZE])
        {
            Sha256 hash;
            hash.Update(data, size);
            hash.Final(digest);
        }

        void HmacSha256(const uint8_t *key, size_t keySize, const uint8_t *data, size_t size, uint8_t mac[SHA256_SIZE])
        {
            uint8_t block[64] = {0};
            if (keySize > 64) Sha256Digest(key, keySize, block);
            else std::memcpy(block, key, keySize);

            uint8_t pad[64];
            for (int i = 0; i < 64; ++i) pad[i] = block[i] ^ 0x36;
            uint8_t inner[SHA256_SIZE];
            Sha256 innerHash;
            innerHash.Update(pad, 64);
            innerHash.Update(data, size);
            innerHash.Final(inner);

            for (int i = 0; i < 64; ++i) pad[i] = block[i] ^ 0x5c;
            Sha256 outerHash;
            outerHash.Update(pad, 64);
            outerHash.Update(inner, SHA256_SIZE);
            outerHash.Final(mac);
        }

        bool Equal(const uint8_t *a, const uint8_t *b, size_t size)
        {
            uint8_t difference = 0;
            for (size_t i = 0; i < size; ++i) difference |= (uint8_t)(a[i] ^ b[i]);
            return difference == 0;
        }
    }
}
