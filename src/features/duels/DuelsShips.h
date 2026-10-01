#pragma once

#include <cstdint>
#include <string>
#include <vector>

// FTL's player ships by type (roadmap 3.9, the ship choice; 3.6, the AI's ship): the ten types, each with its
// layouts A, B and C (the Crystal and Lanius ships have two), by their blueprints' names.
namespace Duels
{
    namespace Ships
    {
        static const int TYPE_COUNT = 10;
        static const uint16_t ALL_TYPES = (1 << TYPE_COUNT) - 1;

        // The type's word ("kestrel", for verbs and duels.cfg), its name ("Kestrel"), and the blueprint of its layout A
        // (PLAYER_SHIP_HARD).
        const char *TypeWord(int type);
        const char *TypeName(int type);
        const char *TypeBlueprint(int type);

        // Its layouts' blueprints that the game has, A first ("PLAYER_SHIP_HARD", "PLAYER_SHIP_HARD_2", ...).
        std::vector<std::string> Variants(int type);
        // The type of a player ship's blueprint (-1: none of these).
        int TypeOf(const std::string &blueprint);
        // A type by its word or name ("kestrel", "Kestrel"), or its number from 1.
        bool ParseType(const std::string &word, int &type);
        // A ship by its blueprint (PLAYER_SHIP_HARD_2) or its type and layout ("kestrel-b"; "kestrel" is layout A), if
        // the game has it.
        bool ParseShip(const std::string &word, std::string &blueprint);
        // A set of types ("all", "kestrel,mantis,..."), as bits.
        bool ParseTypes(const std::string &text, uint16_t &types);
        std::string TypesText(uint16_t types);
        // "Kestrel A": the type's name and the layout's letter.
        std::string Title(const std::string &blueprint);
    }
}
