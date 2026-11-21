#include <windows.h>
#include <shellapi.h>
#include <string>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <algorithm>

#include "nexus/Nexus.h"
#include "mumble/Mumble.h"
#include "imgui.h"
#include <nlohmann/json.hpp>

#include "DiscordClient.h"
#include "RichPresence.h"
#include "DiscordIPC.h"
#include "Gw2Roster.h"
#include "VoiceBridge.h"
#include "bridge/PieUIBridge.h"

// Version constants
#define V_MAJOR 0
#define V_MINOR 9
#define V_BUILD 4
#define V_REVISION 0

// Global variables — all POD/trivial to avoid DllMain issues
HMODULE hSelf;
AddonDefinition_t AddonDef{};
AddonAPI_t* APIDefs = nullptr;

// Settings
bool  g_ShowAvatars = true;
bool  g_ShowNames = true;
bool  g_LockPosition = false;
bool  g_ShowChannelName = true;
bool  g_SpeakingOnly = false;
bool  g_HideWhenNotInVoice = false;
bool  g_GrowUpward = false;
bool  g_HideOnHover = false;
float g_OverlayScale = 1.0f;
float g_AvatarSize = 24.0f;
float g_ColBackground[4] = {0.12f, 0.13f, 0.15f, 0.85f};
float g_ColSpeaking[4]   = {0.90f, 0.90f, 0.90f, 1.0f};
float g_ColSilent[4]     = {0.55f, 0.55f, 0.55f, 1.0f};
float g_ColMuted[4]      = {0.93f, 0.26f, 0.26f, 1.0f};
float g_ColChannel[4]    = {0.34f, 0.40f, 0.96f, 1.0f};

// Which name the overlay shows. GW2 names fall back to the account name,
// then the Discord name, when the player can't be identified.
enum NameMode { NAME_DISCORD = 0, NAME_ACCOUNT = 1, NAME_CHARACTER = 2 };
int   g_NameMode = NAME_DISCORD;

// Saved overlay position. Y is the top edge when growing downward and the
// bottom edge when growing upward, so the fixed edge is what gets restored.
bool  g_HasPos = false;
float g_PosX = 0.0f;
float g_PosY = 0.0f;

static constexpr float AVATAR_SIZE_MIN = 16.0f;
static constexpr float AVATAR_SIZE_MAX = 64.0f;

DiscordClient* g_Discord = nullptr;

// MumbleLink
static Mumble::Data*     g_MumbleLink = nullptr;
static Mumble::Identity  g_MumbleIdentity{};

// Rich Presence
static RichPresence* g_RichPresence = nullptr;
static DiscordIPC*   g_DiscordIPC   = nullptr;
static RPConfig      g_RPConfig{};
static char          g_RPAppIdOverride[64] = {0};

// Config file path (set during load)
static char g_ConfigPath[MAX_PATH] = {0};

// Config is saved shortly after the last change, not on every slider tick
static bool      g_ConfigDirty = false;
static ULONGLONG g_ConfigDirtyAt = 0;

// Frame on which the Nexus options panel last drew our settings
static int g_OptionsFrame = -100;

// Avatar/emoji download bookkeeping and overlay window state.
// Heap-allocated in AddonLoad: no non-trivial globals (DllMain constraint).
struct TexRequest {
    ULONGLONG nextTryAt = 0;
    int attempts = 0;
};
struct OverlayState {
    std::unordered_map<std::string, TexRequest> texRequests;
    std::vector<VoiceUser> visibleUsers;
    std::vector<Texture_t*> userTextures;
    std::vector<std::string> userNames;
    // Manual links from the options panel: Discord user ID -> GW2 account (saved in config)
    std::unordered_map<std::string, std::string> gw2Links;
    ULONGLONG hideUntil = 0;
    bool wasHovered = false;
    ImVec2 lastPos{0, 0};
    ImVec2 lastSize{0, 0};
    bool posApplied = false;
    bool anchorSet = false;
    float anchorBottom = 0.0f;
    int framesShown = 0;
};
static OverlayState* g_Overlay = nullptr;
static bool g_MumbleSubscribed = false;

static void OnMumbleIdentityUpdated(void* eventArgs) {
    if (!eventArgs) return;
    g_MumbleIdentity = *reinterpret_cast<const Mumble::Identity*>(eventArgs);
}

static std::string GetEffectiveAppId() {
    if (g_RPAppIdOverride[0] != '\0')
        return std::string(g_RPAppIdOverride);
    return DISCORD_APP_ID;
}

static void Log(ELogLevel level, const char* msg) {
    if (APIDefs) APIDefs->Log(level, "Discordant", msg);
}

// Forward declarations
void AddonLoad(AddonAPI_t* aApi);
void AddonUnload();
void AddonRender();
void AddonOptions();

// --- Config persistence ---

static void MarkConfigDirty() {
    g_ConfigDirty = true;
    g_ConfigDirtyAt = GetTickCount64();
}

// Read one setting, leaving the default in place if it is missing or the
// wrong type — one bad value must not discard every setting after it.
template <typename T>
static void ReadKey(const nlohmann::json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it == j.end()) return;
    try { out = it->get<T>(); } catch (...) {}
}

static void ReadColor(const nlohmann::json& j, const char* key, float* dst) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != 4) return;
    for (const auto& v : *it)
        if (!v.is_number()) return;
    for (int i = 0; i < 4; i++)
        dst[i] = std::clamp((*it)[i].get<float>(), 0.0f, 1.0f);
}

static void LoadConfig() {
    if (g_ConfigPath[0] == '\0') return;
    std::ifstream f(g_ConfigPath);
    if (!f.good()) return;  // first run

    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        Log(LOGL_WARNING, "config.json is unreadable; using default settings");
        return;
    }

    std::string token;
    ReadKey(j, "access_token", token);
    if (g_Discord && !token.empty()) g_Discord->SetAccessToken(token);

    ReadKey(j, "show_avatars", g_ShowAvatars);
    ReadKey(j, "show_names", g_ShowNames);
    ReadKey(j, "lock_position", g_LockPosition);
    ReadKey(j, "show_channel_name", g_ShowChannelName);
    ReadKey(j, "speaking_only", g_SpeakingOnly);
    ReadKey(j, "hide_when_not_in_voice", g_HideWhenNotInVoice);
    ReadKey(j, "grow_upward", g_GrowUpward);
    ReadKey(j, "hide_on_hover", g_HideOnHover);
    ReadKey(j, "name_mode", g_NameMode);
    g_NameMode = std::clamp(g_NameMode, (int)NAME_DISCORD, (int)NAME_CHARACTER);

    auto links = j.find("gw2_links");
    if (links != j.end() && links->is_object() && g_Overlay) {
        for (auto it = links->begin(); it != links->end(); ++it)
            if (it.value().is_string() && !it.value().get<std::string>().empty())
                g_Overlay->gw2Links[it.key()] = it.value().get<std::string>();
    }

    ReadKey(j, "overlay_scale", g_OverlayScale);
    g_OverlayScale = std::clamp(g_OverlayScale, 0.5f, 2.0f);
    ReadKey(j, "avatar_size", g_AvatarSize);
    g_AvatarSize = std::clamp(g_AvatarSize, AVATAR_SIZE_MIN, AVATAR_SIZE_MAX);

    // Older versions stored only the background alpha
    if (j.contains("col_background")) {
        ReadColor(j, "col_background", g_ColBackground);
    } else {
        float alpha = g_ColBackground[3];
        ReadKey(j, "overlay_alpha", alpha);
        g_ColBackground[3] = std::clamp(alpha, 0.0f, 1.0f);
    }
    ReadColor(j, "col_speaking", g_ColSpeaking);
    ReadColor(j, "col_silent", g_ColSilent);
    ReadColor(j, "col_muted", g_ColMuted);
    ReadColor(j, "col_channel", g_ColChannel);

    if (j.contains("pos_x") && j.contains("pos_y")) {
        float x = g_PosX, y = g_PosY;
        ReadKey(j, "pos_x", x);
        ReadKey(j, "pos_y", y);
        if (std::isfinite(x) && std::isfinite(y)) {
            g_PosX = x;
            g_PosY = y;
            g_HasPos = true;
        }
    }

    ReadKey(j, "rp_enabled", g_RPConfig.enabled);
    ReadKey(j, "rp_show_name", g_RPConfig.showCharName);
    ReadKey(j, "rp_show_map", g_RPConfig.showMap);
    ReadKey(j, "rp_show_prof", g_RPConfig.showProfession);
    std::string appId;
    ReadKey(j, "rp_app_id", appId);
    snprintf(g_RPAppIdOverride, sizeof(g_RPAppIdOverride), "%s", appId.c_str());
}

static void SaveConfig() {
    g_ConfigDirty = false;
    if (g_ConfigPath[0] == '\0') return;
    try {
        nlohmann::json j;
        std::string token = g_Discord ? g_Discord->GetAccessToken() : std::string();
        if (!token.empty())
            j["access_token"] = token;
        j["show_avatars"] = g_ShowAvatars;
        j["show_names"] = g_ShowNames;
        j["lock_position"] = g_LockPosition;
        j["show_channel_name"] = g_ShowChannelName;
        j["speaking_only"] = g_SpeakingOnly;
        j["hide_when_not_in_voice"] = g_HideWhenNotInVoice;
        j["grow_upward"] = g_GrowUpward;
        j["hide_on_hover"] = g_HideOnHover;
        j["name_mode"] = g_NameMode;
        if (g_Overlay) {
            nlohmann::json links = nlohmann::json::object();
            for (const auto& kv : g_Overlay->gw2Links) links[kv.first] = kv.second;
            j["gw2_links"] = links;
        }
        j["overlay_scale"] = g_OverlayScale;
        j["avatar_size"] = g_AvatarSize;
        if (g_HasPos) {
            j["pos_x"] = g_PosX;
            j["pos_y"] = g_PosY;
        }
        j["rp_enabled"]    = g_RPConfig.enabled;
        j["rp_show_name"]  = g_RPConfig.showCharName;
        j["rp_show_map"]   = g_RPConfig.showMap;
        j["rp_show_prof"]  = g_RPConfig.showProfession;
        j["rp_app_id"] = std::string(g_RPAppIdOverride);
        auto color = [](const float* c) { return nlohmann::json{c[0], c[1], c[2], c[3]}; };
        j["col_background"] = color(g_ColBackground);
        j["col_speaking"] = color(g_ColSpeaking);
        j["col_silent"] = color(g_ColSilent);
        j["col_muted"] = color(g_ColMuted);
        j["col_channel"] = color(g_ColChannel);

        // Write a temp file then swap it in, so a crash mid-write can never
        // leave a truncated config (and lose the Discord sign-in).
        std::string tmpPath = std::string(g_ConfigPath) + ".tmp";
        {
            std::ofstream f(tmpPath, std::ios::trunc);
            f << j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
            f.flush();
            if (!f.good()) {
                Log(LOGL_WARNING, "Could not write config.json.tmp; settings not saved");
                return;
            }
        }
        if (!MoveFileExA(tmpPath.c_str(), g_ConfigPath,
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            Log(LOGL_WARNING, "Could not replace config.json; settings not saved");
        }
    } catch (...) {
        Log(LOGL_WARNING, "Saving config.json failed");
    }
}

// DLL entry point
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) hSelf = hModule;
    return TRUE;
}

// --- Overlay Rendering ---

static ImVec4 ToVec4(const float* c) { return ImVec4(c[0], c[1], c[2], c[3]); }

static ImVec4 NameColor(const VoiceUser& user) {
    if (user.deaf || user.mute) return ToVec4(g_ColMuted);
    if (user.speaking)          return ToVec4(g_ColSpeaking);
    return ToVec4(g_ColSilent);
}

static const std::string& DisplayName(const VoiceUser& user) {
    return user.nick.empty() ? user.username : user.nick;
}

static std::string MumbleCharacterName() {
    // Name is a fixed 20-byte field with no guarantee of a terminator
    return std::string(g_MumbleIdentity.Name, strnlen(g_MumbleIdentity.Name, sizeof(g_MumbleIdentity.Name)));
}

// GW2 account of a voice user without manual links: our own account, or one
// written in their Discord nickname / display name ("[TAG] Name.1234").
static std::string AutoGw2Account(const VoiceUser& user, const Gw2RosterSnapshot& roster) {
    if (user.isSelf && !roster.selfAccount.empty()) return roster.selfAccount;
    const std::string* texts[] = {&user.nick, &user.globalName};
    for (const std::string* t : texts)
        if (const Gw2Member* m = Gw2Roster::FindAccountInText(*t, roster.members))
            return m->account;
    for (const std::string* t : texts) {
        std::string account = Gw2Roster::ParseAccountFromText(*t);
        if (!account.empty()) return account;
    }
    return std::string();
}

static std::string Gw2Account(const VoiceUser& user, const Gw2RosterSnapshot& roster) {
    auto link = g_Overlay->gw2Links.find(user.id);
    if (link != g_Overlay->gw2Links.end()) return link->second;
    return AutoGw2Account(user, roster);
}

// Share the voice channel with other addons (Pie UI) every frame, whatever
// the overlay's name mode is.
static void PublishVoiceBridge() {
    bool connected = g_Discord->GetState() == DiscordState::Connected;
    std::vector<VoiceBridge::User> out;
    std::string channel;
    if (connected) {
        VoiceSnapshot snap = g_Discord->GetVoiceSnapshot();
        channel = snap.channelName;
        if (!snap.users.empty()) {
            Gw2RosterSnapshot roster = Gw2Roster::GetSnapshot();
            out.reserve(snap.users.size());
            for (const auto& u : snap.users) {
                VoiceBridge::User b;
                b.account = Gw2Account(u, roster);
                b.discordName = !u.nick.empty() ? u.nick
                              : !u.globalName.empty() ? u.globalName : u.username;
                b.discordId = u.id;
                if (u.speaking) b.flags |= Bridge::DISCORDANT_USER_SPEAKING;
                if (u.mute)     b.flags |= Bridge::DISCORDANT_USER_MUTED;
                if (u.deaf)     b.flags |= Bridge::DISCORDANT_USER_DEAFENED;
                if (u.isSelf)   b.flags |= Bridge::DISCORDANT_USER_SELF;
                if (g_Overlay->gw2Links.count(u.id)) b.flags |= Bridge::DISCORDANT_USER_MANUAL_LINK;
                out.push_back(std::move(b));
            }
        }
    }
    VoiceBridge::Publish(connected, channel, out);
}

// Name shown in the overlay for the chosen mode, falling back from
// character to account to Discord name.
static std::string OverlayName(const VoiceUser& user, const Gw2RosterSnapshot& roster) {
    if (g_NameMode == NAME_DISCORD) return DisplayName(user);
    if (g_NameMode == NAME_CHARACTER && user.isSelf) {
        std::string name = MumbleCharacterName();
        if (!name.empty()) return name;
    }
    std::string account = Gw2Account(user, roster);
    if (account.empty()) return DisplayName(user);
    if (g_NameMode == NAME_CHARACTER) {
        std::string key = Gw2Roster::NormalizeAccount(account);
        for (const auto& m : roster.members)
            if (!m.character.empty() && Gw2Roster::NormalizeAccount(m.account) == key)
                return m.character;
    }
    return account;
}

// First character of a UTF-8 string (1-4 bytes), for the fallback avatar.
static std::string FirstGlyph(const std::string& s) {
    if (s.empty()) return std::string();
    unsigned char c = (unsigned char)s[0];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    return s.substr(0, std::min(n, s.size()));
}

// Returns a loaded texture, or nullptr while it downloads or if it failed.
// Asks Nexus for the pointer each frame rather than caching it, so it can
// never dangle, and retries a failed download only a few times.
static Texture_t* GetRemoteTexture(const std::string& id, const std::string& endpoint) {
    Texture_t* tex = APIDefs->Textures_Get(id.c_str());
    if (tex) return tex->Resource ? tex : nullptr;

    if (g_Overlay->texRequests.size() > 256) g_Overlay->texRequests.clear();
    TexRequest& req = g_Overlay->texRequests[id];
    ULONGLONG now = GetTickCount64();
    if (req.attempts >= 3 || now < req.nextTryAt) return nullptr;
    req.attempts++;
    req.nextTryAt = now + 15000;
    tex = APIDefs->Textures_GetOrCreateFromURL(id.c_str(), "https://cdn.discordapp.com", endpoint.c_str());
    return (tex && tex->Resource) ? tex : nullptr;
}

static Texture_t* GetAvatarTexture(const VoiceUser& user) {
    if (user.avatar.empty() || user.id.empty()) return nullptr;
    // The avatar hash is part of the ID so a changed avatar is fetched afresh
    return GetRemoteTexture("DISCORDANT_AVATAR_" + user.id + "_" + user.avatar,
                            "/avatars/" + user.id + "/" + user.avatar + ".png?size=64");
}

static Texture_t* GetChannelIconTexture(const std::string& iconId) {
    if (iconId.empty()) return nullptr;
    return GetRemoteTexture("DISCORDANT_EMOJI_" + iconId, "/emojis/" + iconId + ".png?size=32");
}

static void RenderUserDot(const VoiceUser& user) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float radius = 4.0f;
    ImVec2 center(pos.x + radius + 1, pos.y + ImGui::GetTextLineHeight() * 0.5f);
    ImVec4 dotCol = NameColor(user);
    ImGui::GetWindowDrawList()->AddCircleFilled(center, radius, ImGui::ColorConvertFloat4ToU32(dotCol));
    ImGui::Dummy(ImVec2(radius * 2 + 4, ImGui::GetTextLineHeight()));
}

static void RenderUserAvatar(const VoiceUser& user, Texture_t* tex, const std::string& name) {
    float size = g_AvatarSize;
    float radius = size * 0.5f;
    float stroke = std::max(1.5f, size / 10.0f);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 center(pos.x + radius, pos.y + radius);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (tex && tex->Resource) {
        dl->AddImageRounded((ImTextureID)tex->Resource, pos, ImVec2(pos.x + size, pos.y + size),
                            ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255), radius);
    } else {
        // Fallback: coloured circle with the first letter of the name
        dl->AddCircleFilled(center, radius, IM_COL32(88, 101, 242, 255), 48);
        std::string letter = FirstGlyph(name);
        if (!letter.empty()) {
            ImVec2 ts = ImGui::CalcTextSize(letter.c_str());
            dl->AddText(ImVec2(center.x - ts.x * 0.5f + 1.0f, center.y - ts.y * 0.5f),
                        IM_COL32(255, 255, 255, 255), letter.c_str());
        }
    }

    if (user.speaking)
        dl->AddCircle(center, radius + stroke * 0.6f, IM_COL32(61, 199, 112, 255), 48, stroke);

    // Muted or deafened: X through the icon (orange for deafened, red for muted)
    if (user.mute || user.deaf) {
        float pad = radius * 0.3f;
        ImVec2 tl(pos.x + pad, pos.y + pad);
        ImVec2 br(pos.x + size - pad, pos.y + size - pad);
        ImU32 xCol = user.deaf ? IM_COL32(250, 168, 18, 255) : IM_COL32(237, 66, 66, 255);
        dl->AddLine(tl, br, xCol, stroke);
        dl->AddLine(ImVec2(br.x, tl.y), ImVec2(tl.x, br.y), xCol, stroke);
    }

    ImGui::Dummy(ImVec2(size + 4, size));
}

// One line of the overlay: avatar or dot, then optionally the name.
static void RenderUserRow(const VoiceUser& user, Texture_t* tex, const std::string& name) {
    if (g_ShowAvatars) RenderUserAvatar(user, tex, name);
    else               RenderUserDot(user);

    if (!g_ShowNames && g_ShowAvatars) return;

    if (g_ShowNames) {
        ImGui::SameLine(0, 4);
        if (g_ShowAvatars) {
            // Vertically centre the text next to the avatar
            float offset = (g_AvatarSize - ImGui::GetTextLineHeight()) * 0.5f;
            if (offset > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offset);
        }
        ImGui::TextColored(NameColor(user), "%s", name.c_str());
    }

    // Without avatars there is no X, so spell out the state
    if (!g_ShowAvatars && (user.deaf || user.mute)) {
        ImGui::SameLine();
        if (user.deaf) ImGui::TextColored(ImVec4(0.98f, 0.66f, 0.07f, 1.0f), "[D]");
        else           ImGui::TextColored(ImVec4(0.93f, 0.26f, 0.26f, 1.0f), "[M]");
    }
}

static void RenderChannelHeader(Texture_t* tex, const std::string& channelName) {
    float iconSize = ImGui::GetTextLineHeight();
    if (tex && tex->Resource) {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddImageRounded(
            (ImTextureID)tex->Resource, pos, ImVec2(pos.x + iconSize, pos.y + iconSize),
            ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255), 2.0f);
        ImGui::Dummy(ImVec2(iconSize, iconSize));
        ImGui::SameLine(0, 4);
    }
    ImGui::TextColored(ToVec4(g_ColChannel), "%s", channelName.c_str());
}

static bool IsMouseOver(ImVec2 pos, ImVec2 size) {
    ImVec2 m = ImGui::GetMousePos();
    return m.x >= pos.x && m.x <= pos.x + size.x && m.y >= pos.y && m.y <= pos.y + size.y;
}

// Pairs every Begin with End and every Push with a Pop, even if something
// in between throws — an unbalanced ImGui stack would itself crash the game.
struct OverlayWindowScope {
    bool open;
    explicit OverlayWindowScope(ImGuiWindowFlags flags) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(g_ColBackground));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        open = ImGui::Begin("##DiscordantOverlay", nullptr, flags);
    }
    ~OverlayWindowScope() {
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }
    OverlayWindowScope(const OverlayWindowScope&) = delete;
    OverlayWindowScope& operator=(const OverlayWindowScope&) = delete;
};

static void RenderVoiceOverlay() {
    OverlayState& ov = *g_Overlay;
    VoiceSnapshot snap = g_Discord->GetVoiceSnapshot();
    const std::string& channelName = snap.channelName;

    // While the options panel is open, show everything so it can be positioned
    bool optionsOpen = ImGui::GetFrameCount() - g_OptionsFrame <= 2;
    bool inVoice = !snap.users.empty() || !channelName.empty();
    bool showChannel = g_ShowChannelName && !channelName.empty();

    ov.visibleUsers.clear();
    for (auto& u : snap.users) {
        if (!g_SpeakingOnly || optionsOpen || u.speaking)
            ov.visibleUsers.push_back(std::move(u));
    }
    const auto& users = ov.visibleUsers;

    bool showPreview = !inVoice && (optionsOpen || (!g_LockPosition && !g_HideWhenNotInVoice));
    if (!showPreview && users.empty() && !showChannel) return;  // nothing to show

    // Resolve textures before drawing
    Texture_t* chanIconTex = showChannel ? GetChannelIconTexture(snap.channelIconId) : nullptr;
    ov.userTextures.clear();
    for (const auto& u : users)
        ov.userTextures.push_back(g_ShowAvatars ? GetAvatarTexture(u) : nullptr);
    Gw2RosterSnapshot roster;
    if (g_NameMode != NAME_DISCORD) roster = Gw2Roster::GetSnapshot();
    ov.userNames.clear();
    for (const auto& u : users)
        ov.userNames.push_back(OverlayName(u, roster));

    // Hide-on-hover: when locked and hovered, hide for 5 seconds
    if (g_HideOnHover && g_LockPosition && !optionsOpen && GetTickCount64() < ov.hideUntil) {
        if (IsMouseOver(ov.lastPos, ov.lastSize))
            ov.hideUntil = GetTickCount64() + 5000;  // still over it: keep hidden
        return;
    }

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (g_LockPosition)
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoMouseInputs;

    // Restore the saved position once, the first time the overlay is shown
    ImVec2 display = ImGui::GetIO().DisplaySize;
    if (!ov.posApplied) {
        ov.posApplied = true;
        if (g_HasPos) {
            float x = g_PosX, y = g_PosY;
            if (display.x > 0 && display.y > 0) {  // keep it on screen after a resolution change
                x = std::clamp(x, 0.0f, std::max(0.0f, display.x - 40.0f));
                y = g_GrowUpward ? std::clamp(y, 40.0f, display.y)
                                 : std::clamp(y, 0.0f, std::max(0.0f, display.y - 40.0f));
            }
            if (g_GrowUpward) {
                ov.anchorBottom = y;
                ov.anchorSet = true;
                y -= 40.0f;  // corrected to the real height once it is known
            }
            ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
        }
    }

    float minWidth = g_ShowNames ? 160.0f : 0.0f;
    float maxWidth = std::max(300.0f, 300.0f * g_OverlayScale);
    float maxHeight = display.y > 0 ? display.y : 2000.0f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(minWidth, 0), ImVec2(maxWidth, maxHeight));

    OverlayWindowScope window(flags);
    if (!window.open) return;

    ImGui::SetWindowFontScale(g_OverlayScale);

    // Remember the window rect for hide-on-hover (NoMouseInputs blocks ImGui hover)
    ov.lastPos = ImGui::GetWindowPos();
    ov.lastSize = ImGui::GetWindowSize();
    if (g_HideOnHover && g_LockPosition && !optionsOpen) {
        bool hovered = IsMouseOver(ov.lastPos, ov.lastSize);
        if (hovered && !ov.wasHovered)
            ov.hideUntil = GetTickCount64() + 5000;
        ov.wasHovered = hovered;
    }

    if (showPreview) {
        if (g_ShowChannelName) {
            ImGui::TextColored(ToVec4(g_ColChannel), "Voice Channel");
            ImGui::Separator();
        }
        // Dummy users to preview size, colour and avatar settings
        VoiceUser dummies[3];
        dummies[0].username = "UserSpeaking";
        dummies[0].speaking = true;
        dummies[1].username = "UserSilent";
        dummies[2].username = "UserMuted";
        dummies[2].mute = true;
        for (const auto& d : dummies)
            RenderUserRow(d, nullptr, d.username);
        ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f),
                           g_LockPosition ? "Preview (unlock to move)" : "Drag to reposition");
    }

    if (showChannel && !g_GrowUpward) {
        RenderChannelHeader(chanIconTex, channelName);
        ImGui::Separator();
    }

    // Render users in reverse order when growing upward
    int userCount = (int)users.size();
    for (int i = 0; i < userCount; i++) {
        int idx = g_GrowUpward ? (userCount - 1 - i) : i;
        RenderUserRow(users[idx], ov.userTextures[idx], ov.userNames[idx]);
    }

    if (showChannel && g_GrowUpward) {
        ImGui::Separator();
        RenderChannelHeader(chanIconTex, channelName);
    }

    // Keep the bottom edge fixed when growing upward
    ImVec2 winPos = ImGui::GetWindowPos();
    float winH = ImGui::GetWindowSize().y;
    if (g_GrowUpward) {
        if (!ov.anchorSet) {
            ov.anchorBottom = winPos.y + winH;
            ov.anchorSet = true;
        } else if (!g_LockPosition && ImGui::IsWindowFocused() && ImGui::IsMouseDragging(0)) {
            ov.anchorBottom = winPos.y + winH;  // user is dragging it
        } else if (winPos.y != ov.anchorBottom - winH) {
            ImGui::SetWindowPos(ImVec2(winPos.x, ov.anchorBottom - winH));
        }
    } else {
        ov.anchorSet = false;
    }

    // Save the position once it settles (after a drag, or a direction change).
    // The first frames are skipped: an auto-sized window has no real size yet.
    if (++ov.framesShown > 3 && !ImGui::IsMouseDown(0)) {
        float x = winPos.x;
        float y = g_GrowUpward ? ov.anchorBottom : winPos.y;
        if (!g_HasPos || std::fabs(x - g_PosX) > 0.5f || std::fabs(y - g_PosY) > 0.5f) {
            g_PosX = x;
            g_PosY = y;
            g_HasPos = true;
            MarkConfigDirty();
        }
    }
}

// --- Addon Lifecycle ---

static void DestroyAll() {
    if (g_MumbleSubscribed) {
        APIDefs->Events_Unsubscribe(EV_MUMBLE_IDENTITY_UPDATED, OnMumbleIdentityUpdated);
        g_MumbleSubscribed = false;
    }
    VoiceBridge::Shutdown();
    Gw2Roster::Shutdown();
    if (g_RichPresence) {
        g_RichPresence->Shutdown();  // queues a presence clear, sent as IPC stops
        delete g_RichPresence;
        g_RichPresence = nullptr;
    }
    if (g_DiscordIPC) {
        g_DiscordIPC->Stop();
        delete g_DiscordIPC;
        g_DiscordIPC = nullptr;
    }
    if (g_Discord) {
        g_Discord->Disconnect();
        delete g_Discord;
        g_Discord = nullptr;
    }
    delete g_Overlay;
    g_Overlay = nullptr;
}

void AddonLoad(AddonAPI_t* aApi) {
    APIDefs = aApi;
    ImGui::SetCurrentContext((ImGuiContext*)APIDefs->ImguiContext);
    ImGui::SetAllocatorFunctions((void* (*)(size_t, void*))APIDefs->ImguiMalloc,
                                 (void(*)(void*, void*))APIDefs->ImguiFree);

    try {
        const char* addonDir = APIDefs->Paths_GetAddonDirectory("Discordant");
        if (addonDir) {
            snprintf(g_ConfigPath, MAX_PATH, "%s\\config.json", addonDir);
            CreateDirectoryA(addonDir, NULL);
        }

        // Heap-allocated, not globals, to keep construction out of DllMain
        g_Overlay = new OverlayState();
        g_Discord = new DiscordClient();
        LoadConfig();  // before starting anything, so saved settings apply from the start

        g_DiscordIPC = new DiscordIPC();
        g_DiscordIPC->Start(GetEffectiveAppId());
        g_RichPresence = new RichPresence(g_DiscordIPC);
        g_MumbleLink = (Mumble::Data*)APIDefs->DataLink_Get(DL_MUMBLE_LINK);
        APIDefs->Events_Subscribe(EV_MUMBLE_IDENTITY_UPDATED, OnMumbleIdentityUpdated);
        g_MumbleSubscribed = true;
        Gw2Roster::Init(APIDefs);
        VoiceBridge::Init(APIDefs);

        g_Discord->Connect();
    } catch (const std::exception& e) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to start: %s", e.what());
        Log(LOGL_CRITICAL, msg);
        try { DestroyAll(); } catch (...) {}
        return;
    } catch (...) {
        Log(LOGL_CRITICAL, "Failed to start");
        try { DestroyAll(); } catch (...) {}
        return;
    }

    APIDefs->GUI_Register(RT_Render, AddonRender);
    APIDefs->GUI_Register(RT_OptionsRender, AddonOptions);
    Log(LOGL_INFO, "Addon loaded");
}

void AddonUnload() {
    // Stop Nexus calling into us before anything is torn down
    APIDefs->GUI_Deregister(AddonOptions);
    APIDefs->GUI_Deregister(AddonRender);

    try {
        if (g_Discord) SaveConfig();
        g_MumbleLink = nullptr;
        DestroyAll();
    } catch (...) {}

    APIDefs = nullptr;
}

// Report an error caught at the Nexus boundary. Limited to a few lines so a
// recurring error cannot flood the log.
static void ReportCaughtError(const char* where, const char* what) {
    static int s_reported = 0;
    if (s_reported >= 5) return;
    s_reported++;
    char msg[512];
    snprintf(msg, sizeof(msg), "Error in %s (handled, game unaffected): %s%s",
             where, what ? what : "unknown",
             s_reported == 5 ? " — further errors will not be logged" : "");
    Log(LOGL_WARNING, msg);
}

static void DrainLogs() {
    for (const auto& msg : g_Discord->DrainLogQueue())
        APIDefs->Log(LOGL_INFO, "Discordant", msg.c_str());
    if (g_DiscordIPC)
        for (const auto& msg : g_DiscordIPC->DrainLogQueue())
            APIDefs->Log(LOGL_INFO, "Discordant/IPC", msg.c_str());
    if (g_RichPresence)
        for (const auto& msg : g_RichPresence->DrainLogQueue())
            APIDefs->Log(LOGL_INFO, "Discordant/RP", msg.c_str());
    for (const auto& msg : Gw2Roster::DrainLogQueue())
        APIDefs->Log(LOGL_INFO, "Discordant/GW2", msg.c_str());
}

static void RenderFrame() {
    if (!g_Discord || !g_Overlay) return;

    Gw2Roster::Update();
    PublishVoiceBridge();

    if (g_RichPresence) {
        RPGameState rpState;
        rpState.inGame = g_MumbleLink && g_MumbleLink->UITick > 0
                         && g_MumbleIdentity.Name[0] != '\0';
        // Name is a fixed 20-byte field with no guarantee of a terminator
        rpState.charName = std::string(g_MumbleIdentity.Name,
                                       strnlen(g_MumbleIdentity.Name, sizeof(g_MumbleIdentity.Name)));
        rpState.mapId          = g_MumbleIdentity.MapID;
        rpState.profession     = static_cast<unsigned>(g_MumbleIdentity.Profession);
        rpState.specialization = g_MumbleIdentity.Specialization;
        g_RichPresence->Update(rpState, g_RPConfig);
    }

    // Flush log queues to Nexus log (throttled to every 30 frames)
    static int s_logFrameCount = 0;
    if (++s_logFrameCount >= 30) {
        s_logFrameCount = 0;
        DrainLogs();
    }

    // Save when the network thread obtains (or discards) a sign-in token,
    // or shortly after a settings change.
    static uint32_t s_savedTokenGen = 0;
    uint32_t tokenGen = g_Discord->GetTokenGeneration();
    if (tokenGen != s_savedTokenGen) {
        s_savedTokenGen = tokenGen;
        SaveConfig();
    } else if (g_ConfigDirty && GetTickCount64() - g_ConfigDirtyAt > 500) {
        SaveConfig();
    }

    if (g_Discord->GetState() == DiscordState::Connected)
        RenderVoiceOverlay();
}

// Nexus calls these from the game's render thread. A C++ exception escaping
// here terminates Guild Wars 2, so nothing is allowed through.
void AddonRender() {
    try {
        RenderFrame();
    } catch (const std::exception& e) {
        ReportCaughtError("render", e.what());
    } catch (...) {
        ReportCaughtError("render", nullptr);
    }
}

// "Player names" section of the options panel. Returns true if a setting changed.
static bool PlayerNamesOptions() {
    bool changed = false;
    ImGui::Text("Show names as");
    if (ImGui::RadioButton("Discord name", g_NameMode == NAME_DISCORD))     { g_NameMode = NAME_DISCORD; changed = true; }
    ImGui::SameLine();
    if (ImGui::RadioButton("GW2 account", g_NameMode == NAME_ACCOUNT))      { g_NameMode = NAME_ACCOUNT; changed = true; }
    ImGui::SameLine();
    if (ImGui::RadioButton("GW2 character", g_NameMode == NAME_CHARACTER))  { g_NameMode = NAME_CHARACTER; changed = true; }
    ImGui::TextDisabled("Players are recognised when their Discord nickname contains their account name\n"
                        "(e.g. Name.1234), or when linked below. Unrecognised players keep their Discord name;\n"
                        "a character name needs the player to be in your party or squad.");

    Gw2RosterSnapshot roster = Gw2Roster::GetSnapshot();
    static const char* kSourceNames[SRC_COUNT] = {"RTAPI", "ArcDPS", "Unofficial Extras", "Pie UI"};
    ImGui::BeginGroup();
    ImGui::Text("Squad data:");
    for (int i = 0; i < SRC_COUNT; i++) {
        ImGui::SameLine();
        if (roster.sourceActive[i]) ImGui::TextColored(ImVec4(0.24f, 0.78f, 0.44f, 1.0f), "%s", kSourceNames[i]);
        else                        ImGui::TextDisabled("%s", kSourceNames[i]);
    }
    ImGui::EndGroup();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Green: receiving data. Grey: not running, or nothing received yet\n"
                          "(ArcDPS and Unofficial Extras report only once you are in a group).");

    if (ImGui::TreeNode("Link Discord users to GW2 accounts")) {
        VoiceSnapshot snap = g_Discord->GetVoiceSnapshot();
        if (snap.users.empty())
            ImGui::TextDisabled("Join a voice channel to link its members.");
        for (const auto& u : snap.users) {
            if (u.isSelf || u.id.empty()) continue;
            ImGui::PushID(u.id.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(DisplayName(u).c_str());
            ImGui::SameLine(220.0f);

            auto link = g_Overlay->gw2Links.find(u.id);
            bool linked = link != g_Overlay->gw2Links.end();
            std::string preview;
            if (linked) {
                preview = link->second;
            } else {
                std::string autoAccount = AutoGw2Account(u, roster);
                preview = autoAccount.empty() ? "Automatic (not recognised)" : "Automatic: " + autoAccount;
            }

            ImGui::SetNextItemWidth(280.0f);
            if (ImGui::BeginCombo("##link", preview.c_str())) {
                if (ImGui::Selectable("Automatic", !linked)) {
                    g_Overlay->gw2Links.erase(u.id);
                    changed = true;
                }
                for (const auto& m : roster.members) {
                    std::string label = m.character.empty() ? m.account : m.account + " (" + m.character + ")";
                    bool selected = linked && Gw2Roster::NormalizeAccount(link->second) == Gw2Roster::NormalizeAccount(m.account);
                    if (ImGui::Selectable(label.c_str(), selected)) {
                        g_Overlay->gw2Links[u.id] = m.account;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        if (roster.members.empty())
            ImGui::TextDisabled("Join a party or squad with someone to pick their account here.");

        // Links are kept for people who have left the channel, so offer a way to clear them all
        unsigned linkCount = (unsigned)g_Overlay->gw2Links.size();
        if (linkCount > 0) {
            char label[64];
            snprintf(label, sizeof(label), "Forget all links (%u)", linkCount);
            if (ImGui::Button(label))
                ImGui::OpenPopup("##forget_links");
            if (ImGui::BeginPopup("##forget_links")) {
                ImGui::Text("Forget all %u saved links? This cannot be undone.", linkCount);
                if (ImGui::Button("Forget")) {
                    g_Overlay->gw2Links.clear();
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
        ImGui::TreePop();
    }
    return changed;
}

static void OptionsPanel() {
    if (!g_Discord) return;
    g_OptionsFrame = ImGui::GetFrameCount();

    ImGui::Text("Discordant - Discord Voice Overlay");
    if (ImGui::SmallButton("Homepage"))
        ShellExecuteA(NULL, "open", "https://pie.rocks.cc/", NULL, NULL, SW_SHOWNORMAL);
    ImGui::SameLine();
    if (ImGui::SmallButton("Buy me a coffee!"))
        ShellExecuteA(NULL, "open", "https://ko-fi.com/pieorcake", NULL, NULL, SW_SHOWNORMAL);
    ImGui::Spacing();

    DiscordState state = g_Discord->GetState();
    ImVec4 statusColor(0.98f, 0.66f, 0.07f, 1.0f);
    if (state == DiscordState::Connected)         statusColor = ImVec4(0.24f, 0.78f, 0.44f, 1.0f);
    else if (state == DiscordState::Disconnected) statusColor = ImVec4(0.93f, 0.26f, 0.26f, 1.0f);
    ImGui::TextColored(statusColor, "Status: %s", g_Discord->GetStatusText().c_str());

    ImGui::Spacing();
    ImGui::Text("The voice overlay appears automatically when in a Discord voice channel.");
    ImGui::TextDisabled("While this panel is open the overlay is always shown, so you can position it.");
    ImGui::Spacing();

    bool changed = false;
    changed |= ImGui::Checkbox("Show user avatars", &g_ShowAvatars);
    if (g_ShowAvatars) {
        ImGui::Indent();
        int size = (int)(g_AvatarSize + 0.5f);
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::SliderInt("Avatar size", &size, (int)AVATAR_SIZE_MIN, (int)AVATAR_SIZE_MAX, "%d px")) {
            g_AvatarSize = (float)size;
            changed = true;
        }
        ImGui::Unindent();
    }
    changed |= ImGui::Checkbox("Show names", &g_ShowNames);
    changed |= ImGui::Checkbox("Show channel name", &g_ShowChannelName);
    changed |= ImGui::Checkbox("Only show people who are speaking", &g_SpeakingOnly);
    changed |= ImGui::Checkbox("Hide when not in a voice channel", &g_HideWhenNotInVoice);
    changed |= ImGui::Checkbox("Lock overlay position", &g_LockPosition);
    changed |= ImGui::Checkbox("Hide overlay on mouseover (5s)", &g_HideOnHover);

    ImGui::Text("List grow direction");
    if (ImGui::RadioButton("Downward", !g_GrowUpward)) { g_GrowUpward = false; changed = true; }
    ImGui::SameLine();
    if (ImGui::RadioButton("Upward", g_GrowUpward)) { g_GrowUpward = true; changed = true; }

    ImGui::Text("Font scale");
    int scalePct = (int)(g_OverlayScale * 100.0f + 0.5f);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::SliderInt("##scale", &scalePct, 50, 200, "%d%%")) {
        g_OverlayScale = scalePct / 100.0f;
        changed = true;
    }

    ImGui::Text("Colours");
    const ImGuiColorEditFlags colFlags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar;
    changed |= ImGui::ColorEdit4("Background##col", g_ColBackground, colFlags);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Set the alpha (A) to 0 for a fully transparent background.");
    changed |= ImGui::ColorEdit4("Speaking##col", g_ColSpeaking, colFlags);
    changed |= ImGui::ColorEdit4("Silent##col", g_ColSilent, colFlags);
    changed |= ImGui::ColorEdit4("Muted##col", g_ColMuted, colFlags);
    changed |= ImGui::ColorEdit4("Channel name##col", g_ColChannel, colFlags);

    ImGui::Separator();
    ImGui::Text("Player names");
    ImGui::Spacing();
    changed |= PlayerNamesOptions();

    ImGui::Separator();
    ImGui::Text("Rich Presence");
    ImGui::Spacing();

    changed |= ImGui::Checkbox("Enable Rich Presence", &g_RPConfig.enabled);
    if (g_RPConfig.enabled) {
        ImGui::Indent();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("Application ID", g_RPAppIdOverride, sizeof(g_RPAppIdOverride),
                         ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Your Discord Application ID. Leave blank to use the built-in ID.");
        // Apply when editing finishes, not on every keystroke (each change reconnects)
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (g_DiscordIPC) g_DiscordIPC->SetAppId(GetEffectiveAppId());
            changed = true;
        }
        changed |= ImGui::Checkbox("Show character name", &g_RPConfig.showCharName);
        changed |= ImGui::Checkbox("Show current map",    &g_RPConfig.showMap);
        changed |= ImGui::Checkbox("Show profession",     &g_RPConfig.showProfession);
        ImGui::Unindent();
    }

    if (changed) MarkConfigDirty();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (state == DiscordState::Disconnected) {
        if (ImGui::Button("Connect to Discord"))
            g_Discord->Connect();
    } else {
        if (ImGui::Button("Disconnect from Discord"))
            g_Discord->Disconnect();
    }
}

void AddonOptions() {
    try {
        OptionsPanel();
    } catch (const std::exception& e) {
        ReportCaughtError("options", e.what());
    } catch (...) {
        ReportCaughtError("options", nullptr);
    }
}

// --- Export: GetAddonDef ---

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef() {
    AddonDef.Signature = 0xD15C04DA;
    AddonDef.APIVersion = NEXUS_API_VERSION;
    AddonDef.Name = "Discordant";
    AddonDef.Version.Major = V_MAJOR;
    AddonDef.Version.Minor = V_MINOR;
    AddonDef.Version.Build = V_BUILD;
    AddonDef.Version.Revision = V_REVISION;
    AddonDef.Author = "discordant";
    AddonDef.Description = "Discord overlay and rich presence.";
    AddonDef.Load = AddonLoad;
    AddonDef.Unload = AddonUnload;
    AddonDef.Flags = AF_None;
    AddonDef.Provider = UP_GitHub;
    AddonDef.UpdateLink = "https://github.com/PieOrCake/discordant";

    return &AddonDef;
}
