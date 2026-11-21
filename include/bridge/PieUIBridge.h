// Shared data between Pie UI and Discordant, through Nexus DataLink.
// Each block has exactly one writer. Fields are only ever appended: readers
// index entries with EntriesOffset/EntrySize and check HeaderSize/EntrySize
// before reading a field.
#pragma once
#include <cstdint>

namespace Bridge {

constexpr uint32_t kVersion = 1;

constexpr uint32_t kPieUISignature      = 0x50494555;
constexpr uint32_t kDiscordantSignature = 0xD15C04DA;

struct Header {
    uint32_t HeaderSize;     // sizeof(the full header struct) as written by the owner
    uint32_t Version;        // kVersion
    uint32_t EntrySize;      // sizeof(one entry) as written by the owner
    uint32_t EntriesOffset;  // byte offset of entry 0 from the start of the block
    uint32_t MaxEntries;     // capacity of the block
    uint32_t Count;          // valid entries
    uint32_t Active;         // 1 = owner is producing data
    uint32_t Sequence;       // +1 on every change
};

// ---- PIEUI_GROUP: written by Pie UI ----------------------------------------

constexpr const char* DL_PIEUI_GROUP = "PIEUI_GROUP";
constexpr uint32_t    PIEUI_GROUP_MAX = 64;

enum : uint32_t {  // GroupType, same values as RTAPI::EGroupType
    PIEUI_GROUP_NONE      = 0,
    PIEUI_GROUP_PARTY     = 1,
    PIEUI_GROUP_RAIDSQUAD = 2,
    PIEUI_GROUP_SQUAD     = 3,
};

enum : uint32_t {  // PieUIGroupMember::Flags
    PIEUI_MEMBER_SELF        = 1u << 0,
    PIEUI_MEMBER_COMMANDER   = 1u << 1,
    PIEUI_MEMBER_LIEUTENANT  = 1u << 2,
    PIEUI_MEMBER_IN_INSTANCE = 1u << 3,
};

struct PieUIGroupHeader {
    Header   Base;
    uint32_t GroupType;
    uint32_t Reserved;
};

struct PieUIGroupMember {
    char     AccountName[140];    // "Name.1234", no leading ':'
    char     CharacterName[140];  // empty if unknown
    uint32_t Profession;          // 1-9, 0 = unknown
    uint32_t EliteSpecialization; // 0 = unknown / core
    uint32_t Subgroup;            // 0 for parties, 1-15 in squads
    uint32_t Flags;
};

// ---- DISCORDANT_VOICE: written by Discordant -------------------------------

constexpr const char* DL_DISCORDANT_VOICE  = "DISCORDANT_VOICE";
constexpr uint32_t    DISCORDANT_VOICE_MAX = 64;

enum : uint32_t {  // DiscordantVoiceUser::Flags
    DISCORDANT_USER_SPEAKING    = 1u << 0,
    DISCORDANT_USER_MUTED       = 1u << 1,
    DISCORDANT_USER_DEAFENED    = 1u << 2,
    DISCORDANT_USER_SELF        = 1u << 3,
    DISCORDANT_USER_MANUAL_LINK = 1u << 4,
};

struct DiscordantVoiceHeader {
    Header Base;
    char   ChannelName[128];  // empty when not in voice
};

struct DiscordantVoiceUser {
    char     AccountName[140];  // matched GW2 account, or empty
    char     DiscordName[128];  // nickname, else display name, else username
    char     DiscordUserId[24]; // decimal snowflake
    uint32_t Flags;
};

// Entries start at HeaderSize rounded up to 8.
constexpr uint32_t EntriesOffsetFor(uint32_t headerSize) { return (headerSize + 7u) & ~7u; }

}  // namespace Bridge
