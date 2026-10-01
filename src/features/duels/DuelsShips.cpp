#include "Global.h"
#include "DuelsShips.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace Duels
{
    namespace Ships
    {
        struct TypeInfo
        {
            const char *word, *name, *blueprint;
        };

        // In the hangar's order.
        static const TypeInfo TYPES[TYPE_COUNT] = {
            {"kestrel", "Kestrel", "PLAYER_SHIP_HARD"},      {"stealth", "Stealth", "PLAYER_SHIP_STEALTH"},
            {"mantis", "Mantis", "PLAYER_SHIP_MANTIS"},      {"engi", "Engi", "PLAYER_SHIP_CIRCLE"},
            {"federation", "Federation", "PLAYER_SHIP_FED"}, {"slug", "Slug", "PLAYER_SHIP_JELLY"},
            {"rock", "Rock", "PLAYER_SHIP_ROCK"},            {"zoltan", "Zoltan", "PLAYER_SHIP_ENERGY"},
            {"crystal", "Crystal", "PLAYER_SHIP_CRYSTAL"},   {"lanius", "Lanius", "PLAYER_SHIP_ANAEROBIC"},
        };
        static const char *const LAYOUTS[] = {"", "_2", "_3"};

        static std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return text;
        }

        const char *TypeWord(int type)
        {
            return type >= 0 && type < TYPE_COUNT ? TYPES[type].word : "";
        }

        const char *TypeName(int type)
        {
            return type >= 0 && type < TYPE_COUNT ? TYPES[type].name : "";
        }

        const char *TypeBlueprint(int type)
        {
            return type >= 0 && type < TYPE_COUNT ? TYPES[type].blueprint : "";
        }

        std::vector<std::string> Variants(int type)
        {
            std::vector<std::string> variants;
            BlueprintManager *blueprints = G_->GetBlueprints();
            if (type < 0 || type >= TYPE_COUNT) return variants;
            for (const char *layout : LAYOUTS)
            {
                std::string name = std::string(TYPES[type].blueprint) + layout;
                // FTL gives back its DEFAULT ship for a name it doesn't know.
                ShipBlueprint *bp = blueprints ? blueprints->GetShipBlueprint(name, -1) : nullptr;
                if (bp && bp->blueprintName == name) variants.push_back(name);
            }
            return variants;
        }

        int TypeOf(const std::string &blueprint)
        {
            for (int type = 0; type < TYPE_COUNT; ++type)
            {
                for (const char *layout : LAYOUTS)
                {
                    if (blueprint == std::string(TYPES[type].blueprint) + layout) return type;
                }
            }
            return -1;
        }

        bool ParseType(const std::string &word, int &type)
        {
            std::string text = Lower(word);
            int number = std::atoi(text.c_str());
            if (number >= 1 && number <= TYPE_COUNT && text == std::to_string(number))
            {
                type = number - 1;
                return true;
            }
            // The word, or its start if no other type's word starts so ("fed", "zol"; not "s").
            int found = -1;
            for (int t = 0; t < TYPE_COUNT && !text.empty(); ++t)
            {
                if (text == TYPES[t].word)
                {
                    type = t;
                    return true;
                }
                if (std::string(TYPES[t].word).compare(0, text.size(), text) == 0) found = found < 0 ? t : TYPE_COUNT;
            }
            if (found < 0 || found == TYPE_COUNT) return false;
            type = found;
            return true;
        }

        bool ParseShip(const std::string &word, std::string &blueprint)
        {
            int type = TypeOf(word);
            std::string name = word;
            if (type < 0)
            {
                // "kestrel-b": the type, then the layout's letter.
                std::string text = Lower(word);
                size_t dash = text.find('-');
                int layout = 0;
                if (dash != std::string::npos)
                {
                    if (dash + 2 != text.size() || text[dash + 1] < 'a' || text[dash + 1] > 'c') return false;
                    layout = text[dash + 1] - 'a';
                    text = text.substr(0, dash);
                }
                if (!ParseType(text, type)) return false;
                name = std::string(TYPES[type].blueprint) + LAYOUTS[layout];
            }
            std::vector<std::string> variants = Variants(type);
            if (std::find(variants.begin(), variants.end(), name) == variants.end()) return false;
            blueprint = name;
            return true;
        }

        bool ParseTypes(const std::string &text, uint16_t &types)
        {
            if (Lower(text) == "all")
            {
                types = ALL_TYPES;
                return true;
            }
            uint16_t parsed = 0;
            std::stringstream words(text);
            std::string word;
            while (std::getline(words, word, ','))
            {
                int type;
                if (!ParseType(word, type)) return false;
                parsed |= (uint16_t)(1 << type);
            }
            if (!parsed) return false;
            types = parsed;
            return true;
        }

        std::string TypesText(uint16_t types)
        {
            if ((types & ALL_TYPES) == ALL_TYPES) return "all";
            std::string text;
            for (int type = 0; type < TYPE_COUNT; ++type)
            {
                if (types & (1 << type)) text += std::string(text.empty() ? "" : ",") + TYPES[type].word;
            }
            return text.empty() ? "none" : text;
        }

        std::string Title(const std::string &blueprint)
        {
            int type = TypeOf(blueprint);
            if (type < 0) return blueprint;
            char layout = blueprint.size() > 2 && blueprint[blueprint.size() - 2] == '_' ? (char)('A' + (blueprint.back() - '1')) : 'A';
            return std::string(TYPES[type].name) + " " + std::string(1, layout);
        }

        static ShipBlueprint *Find(const std::string &blueprint)
        {
            BlueprintManager *blueprints = G_->GetBlueprints();
            ShipBlueprint *bp = blueprints && TypeOf(blueprint) >= 0 ? blueprints->GetShipBlueprint(blueprint, -1) : nullptr;
            return bp && bp->blueprintName == blueprint ? bp : nullptr;
        }

        static std::string Text(const TextString &text)
        {
            return const_cast<TextString &>(text).GetText();
        }

        std::string Image(const std::string &blueprint)
        {
            ShipBlueprint *bp = Find(blueprint);
            return bp ? "customizeUI/miniship_" + bp->imgFile + ".png" : "";
        }

        std::string Arms(const std::string &blueprint)
        {
            ShipBlueprint *bp = Find(blueprint);
            BlueprintManager *blueprints = G_->GetBlueprints();
            if (!bp || !blueprints) return "";
            std::string weapons, drones;
            for (const std::string &name : bp->weapons)
            {
                const WeaponBlueprint *weapon = blueprints->GetWeaponBlueprint(name);
                if (weapon) weapons += (weapons.empty() ? "" : ", ") + Text(weapon->desc.shortTitle);
            }
            for (const std::string &name : bp->drones)
            {
                const DroneBlueprint *drone = blueprints->GetDroneBlueprint(name);
                if (drone) drones += (drones.empty() ? "" : ", ") + Text(drone->desc.shortTitle);
            }
            // The Federation's artillery fires from a system of its own (11), with its weapon in the system's entry.
            auto artillery = bp->systemInfo.find(11);
            if (std::find(bp->systems.begin(), bp->systems.end(), 11) != bp->systems.end() && artillery != bp->systemInfo.end())
            {
                for (const std::string &name : artillery->second.weapon)
                {
                    const WeaponBlueprint *weapon = blueprints->GetWeaponBlueprint(name);
                    std::string title = weapon ? Text(weapon->desc.shortTitle) : "";
                    if (weapon) weapons += (weapons.empty() ? "" : ", ") + title + (Lower(title).find("artillery") == std::string::npos ? " (artillery)" : "");
                }
            }
            if (weapons.empty()) weapons = "No weapons";
            return drones.empty() ? weapons : weapons + "; " + drones;
        }

        std::string Systems(const std::string &blueprint)
        {
            // FTL's systems by id; those every ship has (oxygen, medbay, the subsystems) left out, a clone bay shown.
            static const char *const NAMES[] = {"shields", "engines", nullptr, "weapons", "drones", nullptr, nullptr, nullptr,
                                                nullptr, "teleporter", "cloaking", "artillery", "battery", "clone bay",
                                                "mind control", "hacking"};
            ShipBlueprint *bp = Find(blueprint);
            if (!bp) return "";
            std::string text;
            for (int id = 0; id < (int)(sizeof(NAMES) / sizeof(NAMES[0])); ++id)
            {
                if (!NAMES[id] || std::find(bp->systems.begin(), bp->systems.end(), id) == bp->systems.end()) continue;
                auto info = bp->systemInfo.find(id);
                int level = info != bp->systemInfo.end() ? info->second.powerLevel : 1;
                text += std::string(text.empty() ? "" : ", ") + NAMES[id] + " " + std::to_string(level);
            }
            if (std::find(bp->systems.begin(), bp->systems.end(), 0) == bp->systems.end()) text = "no shields" + (text.empty() ? "" : ", " + text);
            if (!text.empty()) text[0] = (char)std::toupper((unsigned char)text[0]);
            return text;
        }

        std::string Augments(const std::string &blueprint)
        {
            ShipBlueprint *bp = Find(blueprint);
            BlueprintManager *blueprints = G_->GetBlueprints();
            if (!bp || !blueprints) return "";
            std::string text;
            for (const std::string &name : bp->augments)
            {
                const AugmentBlueprint *augment = blueprints->GetAugmentBlueprint(name);
                if (augment) text += (text.empty() ? "" : ", ") + Text(augment->desc.title);
            }
            return text;
        }

        std::string CrewText(const std::string &blueprint)
        {
            ShipBlueprint *bp = Find(blueprint);
            if (!bp) return "";
            std::vector<std::string> species = bp->defaultCrew;
            if (species.empty())
            {
                for (const CrewBlueprint &crew : bp->customCrew) species.push_back(crew.name);
            }
            // By species, the most first ("3 crew: 2 engi, 1 human").
            std::vector<std::pair<int, std::string>> counts;
            for (const std::string &name : species)
            {
                auto found = std::find_if(counts.begin(), counts.end(), [&](const std::pair<int, std::string> &c) { return c.second == name; });
                if (found != counts.end()) ++found->first;
                else counts.push_back(std::make_pair(1, name));
            }
            std::stable_sort(counts.begin(), counts.end(), [](const std::pair<int, std::string> &a, const std::pair<int, std::string> &b) { return a.first > b.first; });
            std::string text = std::to_string(species.size()) + " crew";
            for (size_t i = 0; i < counts.size(); ++i)
            {
                std::string name = counts[i].second == "anaerobic" ? "lanius" : counts[i].second == "energy" ? "zoltan" : counts[i].second;
                text += (i == 0 ? ": " : ", ") + std::to_string(counts[i].first) + " " + name;
            }
            return text;
        }
    }
}
