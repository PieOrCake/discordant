#include "VoiceBridge.h"

#include <windows.h>
#include <algorithm>
#include <cstring>

#include "nexus/Nexus.h"
#include "bridge/PieUIBridge.h"

namespace {

using namespace Bridge;

constexpr uint32_t kEntriesOffset = EntriesOffsetFor(sizeof(DiscordantVoiceHeader));
constexpr size_t   kBlockSize = kEntriesOffset + (size_t)DISCORDANT_VOICE_MAX * sizeof(DiscordantVoiceUser);

// Plain pointers: no non-trivial globals (DllMain constraint).
uint8_t* g_block = nullptr;    // shared with other addons
uint8_t* g_scratch = nullptr;  // next state, compared against g_block

// Copy UTF-8 into a fixed buffer, cutting at a character boundary.
void CopyUtf8(char* dst, size_t size, const std::string& src) {
    size_t n = std::min(src.size(), size - 1);
    if (n < src.size())
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

void WriteHeader(uint8_t* block, bool active, uint32_t count, uint32_t sequence) {
    auto* h = reinterpret_cast<DiscordantVoiceHeader*>(block);
    h->Base.HeaderSize    = sizeof(DiscordantVoiceHeader);
    h->Base.Version       = kVersion;
    h->Base.EntrySize     = sizeof(DiscordantVoiceUser);
    h->Base.EntriesOffset = kEntriesOffset;
    h->Base.MaxEntries    = DISCORDANT_VOICE_MAX;
    h->Base.Count         = count;
    h->Base.Active        = active ? 1 : 0;
    h->Base.Sequence      = sequence;
}

}  // namespace

namespace VoiceBridge {

void Init(AddonAPI_t* api) {
    if (g_block || !api || !api->DataLink_Share) return;
    g_block = static_cast<uint8_t*>(api->DataLink_Share(DL_DISCORDANT_VOICE, kBlockSize));
    if (!g_block) return;
    g_scratch = new uint8_t[kBlockSize];
    memset(g_block, 0, kBlockSize);
    WriteHeader(g_block, false, 0, 0);
}

void Publish(bool active, const std::string& channelName, const std::vector<User>& users) {
    if (!g_block) return;
    memset(g_scratch, 0, kBlockSize);
    uint32_t count = active ? (uint32_t)std::min<size_t>(users.size(), DISCORDANT_VOICE_MAX) : 0;
    for (uint32_t i = 0; i < count; i++) {
        auto* e = reinterpret_cast<DiscordantVoiceUser*>(g_scratch + kEntriesOffset + i * sizeof(DiscordantVoiceUser));
        CopyUtf8(e->AccountName, sizeof(e->AccountName), users[i].account);
        CopyUtf8(e->DiscordName, sizeof(e->DiscordName), users[i].discordName);
        CopyUtf8(e->DiscordUserId, sizeof(e->DiscordUserId), users[i].discordId);
        e->Flags = users[i].flags;
    }
    auto* h = reinterpret_cast<DiscordantVoiceHeader*>(g_scratch);
    if (active) CopyUtf8(h->ChannelName, sizeof(h->ChannelName), channelName);

    // Compare with Sequence equal on both sides, then bump it if anything differs
    uint32_t sequence = reinterpret_cast<const Header*>(g_block)->Sequence;
    WriteHeader(g_scratch, active, count, sequence);
    if (memcmp(g_scratch, g_block, kBlockSize) == 0) return;
    reinterpret_cast<Header*>(g_scratch)->Sequence = sequence + 1;
    memcpy(g_block, g_scratch, kBlockSize);
}

void Shutdown() {
    if (!g_block) return;
    auto* h = reinterpret_cast<Header*>(g_block);
    h->Active = 0;
    h->Count = 0;
    h->Sequence++;
    g_block = nullptr;  // owned by Nexus
    delete[] g_scratch;
    g_scratch = nullptr;
}

}  // namespace VoiceBridge
