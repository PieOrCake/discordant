#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace MapData {

struct MapEntry {
    uint32_t    id;
    const char* name;
};

struct MapGroup {
    const char*           groupName;
    std::vector<MapEntry> maps;
};

inline const std::vector<MapGroup>& GetMapGroups() {
    static const std::vector<MapGroup> kGroups = {
        { "Core Tyria", {
            { 15u,   "Queensdale" },
            { 17u,   "Harathi Hinterlands" },
            { 18u,   "Divinity's Reach" },
            { 23u,   "Kessex Hills" },
            { 24u,   "Gendarran Fields" },
            { 50u,   "Lion's Arch" },
            { 73u,   "Bloodtide Coast" },
            { 335u,  "Claw Island" },
            { 873u,  "Southsun Cove" },
            { 929u,  "The Crown Pavilion" },
            { 1155u, "Lion's Arch Aerodrome" },
            { 1483u, "Memory of Old Lion's Arch" },
            { 19u,   "Plains of Ashford" },
            { 20u,   "Blazeridge Steppes" },
            { 21u,   "Fields of Ruin" },
            { 22u,   "Fireheart Rise" },
            { 25u,   "Iron Marches" },
            { 32u,   "Diessa Plateau" },
            { 218u,  "Black Citadel" },
            { 26u,   "Dredgehaunt Cliffs" },
            { 27u,   "Lornar's Pass" },
            { 28u,   "Wayfarer Foothills" },
            { 29u,   "Timberline Falls" },
            { 30u,   "Frostgorge Sound" },
            { 31u,   "Snowden Drifts" },
            { 326u,  "Hoelbrak" },
            { 51u,   "Straits of Devastation" },
            { 62u,   "Cursed Shore" },
            { 65u,   "Malchor's Leap" },
            { 34u,   "Caledon Forest" },
            { 35u,   "Metrica Province" },
            { 39u,   "Mount Maelstrom" },
            { 53u,   "Sparkfly Fen" },
            { 54u,   "Brisban Wildlands" },
            { 91u,   "The Grove" },
            { 139u,  "Rata Sum" },
            { 922u,  "Labyrinthine Cliffs" },
        }},
        { "Living World Season 2", {
            { 988u,  "Dry Top" },
            { 1015u, "The Silverwastes" },
        }},
        { "Heart of Thorns", {
            { 1042u, "Verdant Brink" },
            { 1052u, "Verdant Brink" },
            { 1043u, "Auric Basin" },
            { 1045u, "Tangled Depths" },
            { 1041u, "Dragon's Stand" },
            { 1095u, "Dragon's Stand (Heart of Thorns)" },
            { 1158u, "Noble's Folly" },
        }},
        { "Living World Season 3", {
            { 1165u, "Bloodstone Fen" },
            { 1175u, "Ember Bay" },
            { 1178u, "Bitterfrost Frontier" },
            { 1185u, "Lake Doric" },
            { 1195u, "Draconis Mons" },
            { 1203u, "Siren's Landing" },
        }},
        { "Path of Fire", {
            { 1210u, "Crystal Oasis" },
            { 1211u, "Desert Highlands" },
            { 1215u, "Windswept Haven" },
            { 1226u, "The Desolation" },
            { 1228u, "Elon Riverlands" },
            { 1248u, "Domain of Vabbi" },
        }},
        { "Living World Season 4", {
            { 1263u, "Domain of Istan" },
            { 1271u, "Sandswept Isles" },
            { 1288u, "Domain of Kourna" },
            { 1301u, "Jahai Bluffs" },
            { 1310u, "Thunderhead Peaks" },
            { 1317u, "Dragonfall" },
        }},
        { "Icebrood Saga", {
            { 1330u, "Grothmar Valley" },
            { 1343u, "Bjora Marches" },
            { 1370u, "Eye of the North" },
            { 1371u, "Drizzlewood Coast" },
        }},
        { "End of Dragons", {
            { 1422u, "Dragon's End" },
            { 1428u, "Arborstone" },
            { 1438u, "New Kaineng City" },
            { 1442u, "Seitung Province" },
            { 1452u, "The Echovald Wilds" },
            { 1465u, "Thousand Seas Pavilion" },
            { 1490u, "Gyala Delve" },
        }},
        { "Secrets of the Obscure", {
            { 1509u, "The Wizard's Tower" },
            { 1510u, "Skywatch Archipelago" },
            { 1517u, "Amnytas" },
            { 1526u, "Inner Nayos" },
            { 1593u, "Starlit Weald" },
            { 1595u, "Shipwreck Strand" },
            { 1609u, "Guardian's Glade" },
            { 1596u, "Comosus Isle" },
        }},
        { "Janthir Wilds", {
            { 1550u, "Lowland Shore" },
            { 1554u, "Janthir Syntri" },
            { 1574u, "Bava Nisos" },
            { 1575u, "Mistburned Barrens" },
            { 1558u, "Hearth's Glow" },
            { 1557u, "Abandoned Homestead" },
        }},
        { "New Maps", {
            { 1622u, "Eternity's Garden" },
        }},
        { "Guild Halls", {
            { 1068u, "Gilded Hollow" },
            { 1069u, "Lost Precipice" },
            { 1419u, "Isle of Reflection" },
            { 1315u, "Armistice Bastion" },
        }},
        { "WvW", {
            { 38u,   "Eternal Battlegrounds" },
            { 95u,   "Alpine Borderlands" },
            { 96u,   "Alpine Borderlands" },
            { 968u,  "Edge of the Mists" },
            { 1099u, "Desert Borderlands" },
        }},
        { "Convergences", {
            { 1523u, "Convergence: Outer Nayos (Public)" },
            { 1527u, "Convergence: Outer Nayos (Private Squad)" },
            { 1562u, "Convergence: Mount Balrior (Private Squad)" },
            { 1571u, "Convergence: Mount Balrior (Public)" },
        }},
        { "Raids", {
            { 1062u, "Spirit Vale" },
            { 1149u, "Salvation Pass" },
            { 1156u, "Stronghold of the Faithful" },
            { 1188u, "Bastion of the Penitent" },
            { 1264u, "Hall of Chains" },
            { 1303u, "Mythwright Gambit" },
            { 1323u, "The Key of Ahdashim" },
            { 1504u, "Bastion of the Obscure" },
            { 1507u, "Bastion of Strength" },
            { 1512u, "Bastion of the Celestial" },
        }},
        { "Strikes", {
            { 1332u, "Shiverpeaks Pass" },
            { 1339u, "Boneskinner" },
            { 1341u, "Fraenir of Jormag" },
            { 1346u, "Voice of the Fallen and Claw of the Fallen" },
            { 1359u, "Whisper of Jormag" },
            { 1368u, "Forging Steel" },
            { 1374u, "Cold War" },
            { 1409u, "Dragonstorm (Private Squad)" },
            { 1412u, "Dragonstorm" },
            { 1414u, "The Twisted Marionette (Private Squad)" },
            { 1480u, "The Twisted Marionette" },
            { 1432u, "Aetherblade Hideout" },
            { 1437u, "Harvest Temple" },
            { 1450u, "Xunlai Jade Junkyard" },
            { 1451u, "Kaineng Overlook" },
            { 1485u, "Old Lion's Court" },
            { 1515u, "Cosmic Observatory" },
            { 1520u, "Temple of Febe" },
            { 1564u, "Mount Balrior" },
            { 1567u, "Harvest Den" },
            { 1572u, "Balrior Peak: Mount Balrior" },
            { 1583u, "Salvation's Cost: Foundry of Failed Creations" },
            { 1585u, "Salvation's Cost: Saevus's Heart" },
        }},
        { "Mistlock Sanctuary", {
            { 1206u, "Mistlock Sanctuary" },
        }},
        { "Fractals of the Mists", {
            {  872u, "Fractals of the Mists" },
            {  947u, "Volcanic" },
            {  948u, "Uncategorized" },
            {  949u, "Ocean" },
            {  950u, "Swampland" },
            {  951u, "Urban Battleground" },
            {  952u, "Aquatic Ruins" },
            {  953u, "Cliffside" },
            {  954u, "Underground Facility" },
            {  955u, "Molten Furnace" },
            {  956u, "Aetherblade" },
            {  957u, "Thaumanova Reactor" },
            {  958u, "Solid Ocean" },
            {  959u, "Snowblind" },
            {  960u, "Molten Boss" },
            { 1164u, "Chaos" },
            { 1177u, "Nightmare" },
            { 1205u, "Shattered Observatory" },
            { 1267u, "Twilight Oasis" },
            { 1290u, "Deepstone" },
            { 1309u, "Siren's Reef" },
            { 1384u, "Sunqua Peak" },
            { 1500u, "Silent Surf" },
            { 1538u, "Lonely Tower" },
            { 1584u, "Kinfall" },
            { 1590u, "Fractal Incursion Conference" },
        }},
    };
    return kGroups;
}

inline const std::string& GetMapName(uint32_t mapId) {
    static const std::unordered_map<uint32_t, std::string> kFlat = []() {
        std::unordered_map<uint32_t, std::string> m;
        for (const auto& g : GetMapGroups())
            for (const auto& e : g.maps)
                m[e.id] = e.name;
        return m;
    }();
    static const std::string kUnknown = "Unknown Map";
    auto it = kFlat.find(mapId);
    return it != kFlat.end() ? it->second : kUnknown;
}

} // namespace MapData
