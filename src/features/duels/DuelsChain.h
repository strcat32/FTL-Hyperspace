#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

// Hash chains for the dodge rolls (roadmap 4.1; DuelsFair.cpp uses them). A chain is a random value hashed again and
// again (SHA-256); its values are shown from the last hashed back to the start, so each is checked against the one
// before it with one hash, and none can be worked out from those already shown. No game headers, so the netcode tests
// build it outside the game (tests/netcode/chain_test.cpp).
namespace Duels
{
    namespace Chain
    {
        static const size_t VALUE_SIZE = 32;
        typedef std::array<uint8_t, VALUE_SIZE> Bytes;

        // Ours: values[0] is the end (shown first), values[i] the i-th value; values[i - 1] = sha256(values[i]).
        struct Own
        {
            std::vector<Bytes> values;

            void Make(uint32_t length, const uint8_t *seed, size_t seedSize);
            bool Ready() const { return values.size() > 1; }
            uint32_t Length() const { return values.empty() ? 0 : (uint32_t)values.size() - 1; }
        };

        // The other side's chain, as far as it has been shown: its values by index (0: the end), each checked.
        struct Known
        {
            bool have = false;
            uint32_t length = 0;
            std::map<uint32_t, Bytes> values;

            void Start(const Bytes &end, uint32_t chainLength);
            // A value shown: true when it follows the chain (hashed down to the nearest value known below it, and the
            // nearest known above hashed down to it); it is known from then on.
            bool Take(uint32_t index, const Bytes &value);
        };

        // A shot's roll, 0 to 99, from the attacker's value and the defender's: sha256 of both, its first four bytes.
        int Roll(const Bytes &shot, const Bytes &verdict);
    }
}
