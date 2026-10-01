#include "DuelsChain.h"
#include "DuelsCrypto.h"

#include <cstring>
#include <iterator>

namespace Duels
{
    namespace Chain
    {
        static void Hash(Bytes &value)
        {
            Crypto::Sha256Digest(value.data(), VALUE_SIZE, value.data());
        }

        void Own::Make(uint32_t length, const uint8_t *seed, size_t seedSize)
        {
            values.assign((size_t)length + 1, Bytes());
            Crypto::Sha256Digest(seed, seedSize, values[length].data());
            for (uint32_t i = length; i > 0; --i)
            {
                values[i - 1] = values[i];
                Hash(values[i - 1]);
            }
        }

        void Known::Start(const Bytes &end, uint32_t chainLength)
        {
            have = true;
            length = chainLength;
            values.clear();
            values[0] = end;
        }

        bool Known::Take(uint32_t index, const Bytes &value)
        {
            if (!have || index == 0 || index > length) return false;
            auto above = values.lower_bound(index);
            if (above != values.end() && above->first == index) return above->second == value;
            auto below = std::prev(above);   // the end (index 0) is always there
            Bytes hashed = value;
            for (uint32_t i = index; i > below->first; --i) Hash(hashed);
            if (hashed != below->second) return false;
            if (above != values.end())
            {
                hashed = above->second;
                for (uint32_t i = above->first; i > index; --i) Hash(hashed);
                if (hashed != value) return false;
            }
            values[index] = value;
            return true;
        }

        int Roll(const Bytes &shot, const Bytes &verdict)
        {
            uint8_t both[2 * VALUE_SIZE], digest[VALUE_SIZE];
            std::memcpy(both, shot.data(), VALUE_SIZE);
            std::memcpy(both + VALUE_SIZE, verdict.data(), VALUE_SIZE);
            Crypto::Sha256Digest(both, sizeof(both), digest);
            uint32_t number = (uint32_t)digest[0] | ((uint32_t)digest[1] << 8) | ((uint32_t)digest[2] << 16) | ((uint32_t)digest[3] << 24);
            return (int)(number % 100);
        }
    }
}
