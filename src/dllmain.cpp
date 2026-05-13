#include <windows.h>
#include <shellapi.h>
#include <string>
#include <cstring>
#include <fstream>
#include <unordered_map>

#include "nexus/Nexus.h"
#include "mumble/Mumble.h"
#include "imgui.h"
#include <nlohmann/json.hpp>

#include "DiscordClient.h"
#include "RichPresence.h"

// Version constants
#define V_MAJOR 0
#define V_MINOR 9
#define V_BUILD 0
#define V_REVISION 3

// Quick Access identifiers
#define QA_ID "QA_DISCORDANT"
#define TEX_ICON "TEX_DISCORDANT_ICON"
#define TEX_ICON_HOVER "TEX_DISCORDANT_ICON_HOVER"

// Valid 32x32 solid RGBA PNG (normal - gray)
static const unsigned char ICON_HEADPHONES[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0x08, 0x06, 0x00, 0x00, 0x00, 0x73, 0x7a, 0x7a,
    0xf4, 0x00, 0x00, 0x00, 0x34, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0xed, 0xd7, 0x31, 0x11, 0x00,
    0x00, 0x08, 0x03, 0xb1, 0xfa, 0xbf, 0x8a, 0xc0, 0x29, 0xc8, 0x60, 0xc9, 0xd0, 0xfd, 0x2f, 0x5b,
    0xd3, 0xce, 0x7e, 0x2e, 0x02, 0x08, 0x10, 0x20, 0x40, 0x80, 0x00, 0x01, 0x02, 0x04, 0x08, 0x10,
    0x20, 0x40, 0x80, 0x00, 0x81, 0x3e, 0xdf, 0xf3, 0x03, 0x17, 0x90, 0xdc, 0x97, 0x75, 0xe5, 0xd9,
    0x54, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};
static const unsigned int ICON_HEADPHONES_size = sizeof(ICON_HEADPHONES);

// Valid 32x32 solid RGBA PNG (hover - bright)
static const unsigned char ICON_HEADPHONES_HOVER[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0x08, 0x06, 0x00, 0x00, 0x00, 0x73, 0x7a, 0x7a,
    0xf4, 0x00, 0x00, 0x00, 0x34, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0xed, 0xd7, 0x31, 0x11, 0x00,
    0x00, 0x0c, 0x02, 0x31, 0xfc, 0x2b, 0x43, 0x44, 0xbd, 0xb4, 0x32, 0xba, 0x64, 0x60, 0xff, 0xcb,
    0x46, 0xda, 0xd9, 0xcf, 0x45, 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x00, 0x01,
    0x02, 0x04, 0x08, 0x10, 0x20, 0xd0, 0xe7, 0x7b, 0x7e, 0xd3, 0xd1, 0xac, 0xc4, 0xb2, 0xa3, 0xa2,
    0xd1, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};
static const unsigned int ICON_HEADPHONES_HOVER_size = sizeof(ICON_HEADPHONES_HOVER);

// Global variables — all POD/trivial to avoid DllMain issues
HMODULE hSelf;
AddonDefinition_t AddonDef{};
AddonAPI_t* APIDefs = nullptr;
bool g_ShowAvatars = true;
bool g_LockPosition = false;
bool g_ShowChannelName = true;
float g_OverlayAlpha = 0.85f;
float g_OverlayScale = 1.0f;
float g_ColSpeaking[4]  = {0.90f, 0.90f, 0.90f, 1.0f};
float g_ColSilent[4]    = {0.55f, 0.55f, 0.55f, 1.0f};
float g_ColMuted[4]     = {0.93f, 0.26f, 0.26f, 1.0f};
float g_ColChannel[4]   = {0.34f, 0.40f, 0.96f, 1.0f};
bool g_GrowUpward = false;
bool g_HideOnHover = false;
DiscordClient* g_Discord = nullptr;

// MumbleLink
static Mumble::Data*     g_MumbleLink = nullptr;
static Mumble::Identity  g_MumbleIdentity{};

// Rich Presence
static RichPresence* g_RichPresence = nullptr;
static RPConfig      g_RPConfig{};
static char          g_RPAppIdOverride[64] = {0};

// Config file path (set during load)
static char g_ConfigPath[MAX_PATH] = {0};

static void OnMumbleIdentityUpdated(void* eventArgs) {
    if (!eventArgs) return;
    g_MumbleIdentity = *reinterpret_cast<const Mumble::Identity*>(eventArgs);
}

// Forward declarations
void AddonLoad(AddonAPI_t* aApi);
void AddonUnload();
void AddonRender();
void AddonOptions();

// --- Config persistence ---

static void LoadConfig() {
    if (!g_Discord) return;
    try {
        std::ifstream f(g_ConfigPath);
        if (f.good()) {
            nlohmann::json j;
            f >> j;
            if (j.contains("access_token")) {
                g_Discord->SetAccessToken(j["access_token"].get<std::string>());
            }
            if (j.contains("show_avatars")) {
                g_ShowAvatars = j["show_avatars"].get<bool>();
            }
            if (j.contains("lock_position")) {
                g_LockPosition = j["lock_position"].get<bool>();
            }
            if (j.contains("show_channel_name")) {
                g_ShowChannelName = j["show_channel_name"].get<bool>();
            }
            if (j.contains("overlay_alpha")) {
                g_OverlayAlpha = j["overlay_alpha"].get<float>();
                if (g_OverlayAlpha < 0.1f) g_OverlayAlpha = 0.1f;
                if (g_OverlayAlpha > 1.0f) g_OverlayAlpha = 1.0f;
            }
            if (j.contains("overlay_scale")) {
                g_OverlayScale = j["overlay_scale"].get<float>();
                if (g_OverlayScale < 0.5f) g_OverlayScale = 0.5f;
                if (g_OverlayScale > 2.0f) g_OverlayScale = 2.0f;
            }
            auto loadCol = [&](const char* key, float* dst) {
                if (j.contains(key) && j[key].is_array() && j[key].size() == 4)
                    for (int i = 0; i < 4; i++) dst[i] = j[key][i].get<float>();
            };
            if (j.contains("grow_upward")) {
                g_GrowUpward = j["grow_upward"].get<bool>();
            }
            if (j.contains("hide_on_hover")) {
                g_HideOnHover = j["hide_on_hover"].get<bool>();
            }
            if (j.contains("rp_enabled"))      g_RPConfig.enabled        = j["rp_enabled"].get<bool>();
            if (j.contains("rp_show_name"))     g_RPConfig.showCharName   = j["rp_show_name"].get<bool>();
            if (j.contains("rp_show_map"))      g_RPConfig.showMap        = j["rp_show_map"].get<bool>();
            if (j.contains("rp_show_prof"))     g_RPConfig.showProfession = j["rp_show_prof"].get<bool>();
            if (j.contains("rp_show_party"))    g_RPConfig.showPartySize  = j["rp_show_party"].get<bool>();
            if (j.contains("rp_app_id")) {
                std::string aid = j["rp_app_id"].get<std::string>();
                strncpy(g_RPAppIdOverride, aid.c_str(), sizeof(g_RPAppIdOverride) - 1);
            }
            loadCol("col_speaking", g_ColSpeaking);
            loadCol("col_silent", g_ColSilent);
            loadCol("col_muted", g_ColMuted);
            loadCol("col_channel", g_ColChannel);
        }
    } catch (...) {}
}

static void SaveConfig() {
    if (!g_Discord) return;
    try {
        nlohmann::json j;
        std::string token = g_Discord->GetAccessToken();
        if (!token.empty()) {
            j["access_token"] = token;
        }
        j["show_avatars"] = g_ShowAvatars;
        j["lock_position"] = g_LockPosition;
        j["show_channel_name"] = g_ShowChannelName;
        j["overlay_alpha"] = g_OverlayAlpha;
        j["overlay_scale"] = g_OverlayScale;
        j["grow_upward"] = g_GrowUpward;
        j["hide_on_hover"] = g_HideOnHover;
        j["rp_enabled"]    = g_RPConfig.enabled;
        j["rp_show_name"]  = g_RPConfig.showCharName;
        j["rp_show_map"]   = g_RPConfig.showMap;
        j["rp_show_prof"]  = g_RPConfig.showProfession;
        j["rp_show_party"] = g_RPConfig.showPartySize;
        j["rp_app_id"] = std::string(g_RPAppIdOverride);
        auto saveCol = [](float* c) -> nlohmann::json { return {c[0], c[1], c[2], c[3]}; };
        j["col_speaking"] = saveCol(g_ColSpeaking);
        j["col_silent"] = saveCol(g_ColSilent);
        j["col_muted"] = saveCol(g_ColMuted);
        j["col_channel"] = saveCol(g_ColChannel);
        std::ofstream f(g_ConfigPath);
        f << j.dump(2);
    } catch (...) {}
}

// DLL entry point
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH: hSelf = hModule; break;
    case DLL_PROCESS_DETACH: break;
    case DLL_THREAD_ATTACH: break;
    case DLL_THREAD_DETACH: break;
    }
    return TRUE;
}

// --- Overlay Rendering ---

static void RenderUserDot(const VoiceUser& user) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float radius = 4.0f;
    ImVec2 center = ImVec2(pos.x + radius + 1, pos.y + ImGui::GetTextLineHeight() * 0.5f);

    ImVec4 dotCol(g_ColSilent[0], g_ColSilent[1], g_ColSilent[2], g_ColSilent[3]);
    if (user.deaf || user.mute) dotCol = ImVec4(g_ColMuted[0], g_ColMuted[1], g_ColMuted[2], g_ColMuted[3]);
    else if (user.speaking)     dotCol = ImVec4(g_ColSpeaking[0], g_ColSpeaking[1], g_ColSpeaking[2], g_ColSpeaking[3]);

    ImGui::GetWindowDrawList()->AddCircleFilled(center, radius, ImGui::ColorConvertFloat4ToU32(dotCol));
    ImGui::Dummy(ImVec2(radius * 2 + 4, 0));
    ImGui::SameLine();
}

// Avatar texture cache: keyed by "userId:avatarHash", value is cached Texture_t*
struct AvatarCacheEntry {
    Texture_t* tex = nullptr;
    bool requested = false;
};
static std::unordered_map<std::string, AvatarCacheEntry> s_avatarCache;

// Channel icon texture cache
static std::string s_chanIconCacheKey;
static AvatarCacheEntry s_chanIconCache;

static Texture_t* GetCachedAvatarTexture(const VoiceUser& user) {
    if (user.avatar.empty()) return nullptr;
    std::string key = user.id + ":" + user.avatar;
    auto it = s_avatarCache.find(key);
    if (it != s_avatarCache.end()) {
        if (it->second.tex && it->second.tex->Resource) return it->second.tex;
        if (it->second.requested && it->second.tex) return nullptr; // pointer exists, Resource still loading
        // tex is null — API returned nullptr while async load was in progress; retry to get pointer
    }
    std::string texId = "TEX_AVATAR_" + user.id;
    std::string endpoint = "/avatars/" + user.id + "/" + user.avatar + ".png?size=32";
    Texture_t* tex = APIDefs->Textures_GetOrCreateFromURL(texId.c_str(), "https://cdn.discordapp.com", endpoint.c_str());
    AvatarCacheEntry& entry = s_avatarCache[key];
    entry.tex = tex;
    entry.requested = true;
    return (tex && tex->Resource) ? tex : nullptr;
}

static Texture_t* GetCachedChannelIconTexture(const std::string& iconId) {
    if (iconId.empty()) return nullptr;
    if (s_chanIconCacheKey == iconId) {
        if (s_chanIconCache.tex && s_chanIconCache.tex->Resource) return s_chanIconCache.tex;
        if (s_chanIconCache.requested && s_chanIconCache.tex) return nullptr; // pointer exists, still loading
        // tex is null — retry to get pointer
    }
    std::string texId = "TEX_CHANICON_" + iconId;
    std::string endpoint = "/emojis/" + iconId + ".png?size=32";
    Texture_t* tex = APIDefs->Textures_GetOrCreateFromURL(texId.c_str(), "https://cdn.discordapp.com", endpoint.c_str());
    s_chanIconCacheKey = iconId;
    s_chanIconCache.tex = tex;
    s_chanIconCache.requested = true;
    return (tex && tex->Resource) ? tex : nullptr;
}

static void RenderUserAvatar(const VoiceUser& user, Texture_t* tex) {
    float iconSize = 24.0f;
    float iconRadius = iconSize * 0.5f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 center = ImVec2(pos.x + iconRadius, pos.y + iconRadius);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (tex && tex->Resource) {
        // Clip avatar to circle using draw list
        ImVec2 uvMin(0, 0);
        ImVec2 uvMax(1, 1);
        ImVec2 pMin = pos;
        ImVec2 pMax = ImVec2(pos.x + iconSize, pos.y + iconSize);

        // Draw circular avatar by rendering as a square image then overlaying
        dl->AddImageRounded(
            (ImTextureID)tex->Resource,
            pMin, pMax, uvMin, uvMax,
            IM_COL32(255, 255, 255, 255),
            iconRadius);
    } else {
        // Fallback: draw a filled circle with first letter
        dl->AddCircleFilled(center, iconRadius, IM_COL32(88, 101, 242, 255), 48);
        std::string displayName = user.nick.empty() ? user.username : user.nick;
        if (!displayName.empty()) {
            char letter[2] = { displayName[0], 0 };
            ImVec2 textSize = ImGui::CalcTextSize(letter);
            dl->AddText(ImVec2(center.x - textSize.x * 0.5f + 1.0f, center.y - textSize.y * 0.5f),
                        IM_COL32(255, 255, 255, 255), letter);
        }
    }

    // Speaking: green border
    if (user.speaking) {
        dl->AddCircle(center, iconRadius + 1.5f, IM_COL32(61, 199, 112, 255), 48, 2.5f);
    }

    // Muted or deafened: red X through the icon
    if (user.mute || user.deaf) {
        float xPad = iconRadius * 0.3f;
        ImVec2 tl(pos.x + xPad, pos.y + xPad);
        ImVec2 br(pos.x + iconSize - xPad, pos.y + iconSize - xPad);
        ImU32 xCol = user.deaf ? IM_COL32(250, 168, 18, 255) : IM_COL32(237, 66, 66, 255);
        dl->AddLine(tl, br, xCol, 2.5f);
        dl->AddLine(ImVec2(br.x, tl.y), ImVec2(tl.x, br.y), xCol, 2.5f);
    }

    ImGui::Dummy(ImVec2(iconSize + 4, iconSize));
    ImGui::SameLine(0, 4);
}

static void RenderChannelIcon(Texture_t* tex, float size) {
    if (!tex || !tex->Resource) return;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddImageRounded(
        (ImTextureID)tex->Resource,
        pos, ImVec2(pos.x + size, pos.y + size),
        ImVec2(0, 0), ImVec2(1, 1),
        IM_COL32(255, 255, 255, 255), 2.0f);
    ImGui::Dummy(ImVec2(size, size));
    ImGui::SameLine(0, 4);
}

static void RenderVoiceOverlay() {
    // Single mutex lock for all shared voice state
    VoiceSnapshot snap = g_Discord->GetVoiceSnapshot();
    auto& users = snap.users;
    auto& channelName = snap.channelName;

    // Pre-resolve textures from cache (avoids per-user string concat in render loop)
    Texture_t* chanIconTex = GetCachedChannelIconTexture(snap.channelIconId);
    static std::vector<Texture_t*> s_userTextures;
    s_userTextures.clear();
    s_userTextures.reserve(users.size());
    for (const auto& u : users) {
        s_userTextures.push_back(GetCachedAvatarTexture(u));
    }

    // Evict stale avatar cache entries for users no longer in channel
    if (s_avatarCache.size() > users.size() + 8) {
        std::unordered_map<std::string, AvatarCacheEntry> fresh;
        for (const auto& u : users) {
            if (!u.avatar.empty()) {
                std::string key = u.id + ":" + u.avatar;
                auto it = s_avatarCache.find(key);
                if (it != s_avatarCache.end()) fresh[key] = it->second;
            }
        }
        s_avatarCache.swap(fresh);
    }

    bool hasContent = !users.empty() || !channelName.empty();
    if (!hasContent && g_LockPosition) return;  // Nothing to show and locked — hide

    // Hide-on-hover: when locked and hovered, hide for 5 seconds
    static ULONGLONG s_hideUntil = 0;
    static bool s_wasHovered = false;
    static ImVec2 s_lastPos(0, 0);
    static ImVec2 s_lastSize(0, 0);
    if (g_HideOnHover && g_LockPosition && GetTickCount64() < s_hideUntil) {
        // Check if mouse is still over where the overlay was — restart timer
        ImVec2 mouse = ImGui::GetMousePos();
        bool stillOver = mouse.x >= s_lastPos.x && mouse.x <= s_lastPos.x + s_lastSize.x &&
                         mouse.y >= s_lastPos.y && mouse.y <= s_lastPos.y + s_lastSize.y;
        if (stillOver) {
            s_hideUntil = GetTickCount64() + 5000;
        }
        return;  // Still hidden
    }

    ImVec4 colBg(0.12f, 0.13f, 0.15f, g_OverlayAlpha);
    ImVec4 colHeader(g_ColChannel[0], g_ColChannel[1], g_ColChannel[2], g_ColChannel[3]);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, colBg);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));

    ImGuiWindowFlags overlayFlags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (g_LockPosition) {
        overlayFlags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoMouseInputs;
    }

    // When growing upward, anchor the bottom edge of the window
    static float s_anchorBottom = -1.0f;
    static bool s_anchorSet = false;
    if (g_GrowUpward && s_anchorSet) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(160, 40), ImVec2(300, 600));
    } else {
        ImGui::SetNextWindowSizeConstraints(ImVec2(160, 40), ImVec2(300, 600));
    }
    if (ImGui::Begin("##DiscordantOverlay", nullptr, overlayFlags))
    {
        ImGui::SetWindowFontScale(g_OverlayScale);

        // Manual hover detection (NoMouseInputs blocks ImGui hover)
        // Save window rect for use when hidden
        s_lastPos = ImGui::GetWindowPos();
        s_lastSize = ImGui::GetWindowSize();
        if (g_HideOnHover && g_LockPosition) {
            ImVec2 mouse = ImGui::GetMousePos();
            bool hovered = mouse.x >= s_lastPos.x && mouse.x <= s_lastPos.x + s_lastSize.x &&
                           mouse.y >= s_lastPos.y && mouse.y <= s_lastPos.y + s_lastSize.y;
            if (hovered && !s_wasHovered) {
                s_hideUntil = GetTickCount64() + 5000;
            }
            s_wasHovered = hovered;
        }
        if (!hasContent) {
            if (g_ShowChannelName) {
                ImGui::TextColored(colHeader, "Voice Channel");
                ImGui::Separator();
            }
            // Dummy users to preview font size, color, and avatar settings
            VoiceUser dummies[3];
            dummies[0].username = "UserSpeaking";
            dummies[0].speaking = true;
            dummies[1].username = "UserSilent";
            dummies[2].username = "UserMuted";
            dummies[2].mute = true;
            for (int i = 0; i < 3; i++) {
                if (g_ShowAvatars) {
                    // Fallback avatar (no real texture)
                    float iconSize = 24.0f;
                    float iconRadius = iconSize * 0.5f;
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImVec2 center(pos.x + iconRadius, pos.y + iconRadius);
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddCircleFilled(center, iconRadius, IM_COL32(88, 101, 242, 255), 48);
                    char letter[2] = { dummies[i].username[0], 0 };
                    ImVec2 ts = ImGui::CalcTextSize(letter);
                    dl->AddText(ImVec2(center.x - ts.x * 0.5f + 1.0f, center.y - ts.y * 0.5f), IM_COL32(255,255,255,255), letter);
                    if (dummies[i].speaking)
                        dl->AddCircle(center, iconRadius + 1.5f, IM_COL32(61, 199, 112, 255), 48, 2.5f);
                    if (dummies[i].mute)  {
                        float xPad = iconRadius * 0.3f;
                        dl->AddLine(ImVec2(pos.x+xPad, pos.y+xPad), ImVec2(pos.x+iconSize-xPad, pos.y+iconSize-xPad), IM_COL32(237,66,66,255), 2.5f);
                        dl->AddLine(ImVec2(pos.x+iconSize-xPad, pos.y+xPad), ImVec2(pos.x+xPad, pos.y+iconSize-xPad), IM_COL32(237,66,66,255), 2.5f);
                    }
                    ImGui::Dummy(ImVec2(iconSize + 4, iconSize));
                    ImGui::SameLine(0, 4);
                } else {
                    RenderUserDot(dummies[i]);
                }
                ImVec4 nc;
                if (dummies[i].mute)          nc = ImVec4(g_ColMuted[0], g_ColMuted[1], g_ColMuted[2], g_ColMuted[3]);
                else if (dummies[i].speaking) nc = ImVec4(g_ColSpeaking[0], g_ColSpeaking[1], g_ColSpeaking[2], g_ColSpeaking[3]);
                else                          nc = ImVec4(g_ColSilent[0], g_ColSilent[1], g_ColSilent[2], g_ColSilent[3]);
                ImGui::TextColored(nc, "%s", dummies[i].username.c_str());
            }
            ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "Drag to reposition");
        }

        if (g_ShowChannelName && !channelName.empty() && !g_GrowUpward) {
            float iconSize = ImGui::GetTextLineHeight();
            RenderChannelIcon(chanIconTex, iconSize);
            ImGui::TextColored(colHeader, "%s", channelName.c_str());
            ImGui::Separator();
        }

        // Render users in reverse order when growing upward
        int userCount = (int)users.size();
        for (int ui = 0; ui < userCount; ui++) {
            int idx = g_GrowUpward ? (userCount - 1 - ui) : ui;
            const auto& user = users[idx];
            if (g_ShowAvatars) {
                RenderUserAvatar(user, s_userTextures[idx]);
            } else {
                RenderUserDot(user);
            }

            std::string displayName = user.nick.empty() ? user.username : user.nick;
            ImVec4 nameColor;
            if (user.deaf || user.mute) nameColor = ImVec4(g_ColMuted[0], g_ColMuted[1], g_ColMuted[2], g_ColMuted[3]);
            else if (user.speaking)     nameColor = ImVec4(g_ColSpeaking[0], g_ColSpeaking[1], g_ColSpeaking[2], g_ColSpeaking[3]);
            else                        nameColor = ImVec4(g_ColSilent[0], g_ColSilent[1], g_ColSilent[2], g_ColSilent[3]);

            if (g_ShowAvatars) {
                // Vertically center text next to avatar
                float offset = (24.0f - ImGui::GetTextLineHeight()) * 0.5f;
                if (offset > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offset);
            }

            ImGui::TextColored(nameColor, "%s", displayName.c_str());

            if (!g_ShowAvatars) {
                ImGui::SameLine();
                if (user.deaf) {
                    ImGui::TextColored(ImVec4(0.98f, 0.66f, 0.07f, 1.0f), "[D]");
                } else if (user.mute) {
                    ImGui::TextColored(ImVec4(0.93f, 0.26f, 0.26f, 1.0f), "[M]");
                }
            }
        }

        if (g_ShowChannelName && !channelName.empty() && g_GrowUpward) {
            ImGui::Separator();
            float iconSize = ImGui::GetTextLineHeight();
            RenderChannelIcon(chanIconTex, iconSize);
            ImGui::TextColored(colHeader, "%s", channelName.c_str());
        }

        // Anchor bottom edge when growing upward
        if (g_GrowUpward) {
            ImVec2 winPos = ImGui::GetWindowPos();
            float winH = ImGui::GetWindowSize().y;
            if (!s_anchorSet) {
                s_anchorBottom = winPos.y + winH;
                s_anchorSet = true;
            } else {
                // If user dragged the window, update anchor
                if (!g_LockPosition && ImGui::IsWindowFocused() && ImGui::IsMouseDragging(0)) {
                    s_anchorBottom = winPos.y + winH;
                } else {
                    float newY = s_anchorBottom - winH;
                    if (newY != winPos.y) {
                        ImGui::SetWindowPos(ImVec2(winPos.x, newY));
                    }
                }
            }
        } else {
            s_anchorSet = false;
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// --- Addon Lifecycle ---

void AddonLoad(AddonAPI_t* aApi) {
    APIDefs = aApi;
    ImGui::SetCurrentContext((ImGuiContext*)APIDefs->ImguiContext);
    ImGui::SetAllocatorFunctions((void* (*)(size_t, void*))APIDefs->ImguiMalloc,
                                 (void(*)(void*, void*))APIDefs->ImguiFree);

    // Allocate Discord client on heap (not as global to avoid DllMain issues)
    g_Discord = new DiscordClient();
    g_RichPresence = new RichPresence(g_Discord);
    g_MumbleLink = (Mumble::Data*)APIDefs->DataLink_Get(DL_MUMBLE_LINK);
    APIDefs->Events_Subscribe("EV_MUMBLE_IDENTITY_UPDATED", OnMumbleIdentityUpdated);

    // Set up config path
    std::string addonDir = APIDefs->Paths_GetAddonDirectory("Discordant");
    snprintf(g_ConfigPath, MAX_PATH, "%s\\config.json", addonDir.c_str());
    CreateDirectoryA(addonDir.c_str(), NULL);

    // Load saved access token
    LoadConfig();

    // Start Discord connection
    g_Discord->Connect();
    APIDefs->Log(LOGL_INFO, "Discordant", "Connect() called");

    // Register render functions
    APIDefs->GUI_Register(RT_Render, AddonRender);
    APIDefs->GUI_Register(RT_OptionsRender, AddonOptions);


    // Load icon textures from embedded PNG data
    APIDefs->Textures_LoadFromMemory(TEX_ICON, (void*)ICON_HEADPHONES, ICON_HEADPHONES_size, nullptr);
    APIDefs->Textures_LoadFromMemory(TEX_ICON_HOVER, (void*)ICON_HEADPHONES_HOVER, ICON_HEADPHONES_HOVER_size, nullptr);

    // Register quick access shortcut
    APIDefs->QuickAccess_Add(QA_ID, TEX_ICON, TEX_ICON_HOVER, "KB_DISCORDANT_TOGGLE", "Discordant");


    APIDefs->Log(LOGL_INFO, "Discordant", "Addon loaded v2 - with logging");
}

void AddonUnload() {
    SaveConfig();

    APIDefs->Events_Unsubscribe("EV_MUMBLE_IDENTITY_UPDATED", OnMumbleIdentityUpdated);
    g_MumbleLink = nullptr;

    if (g_RichPresence) {
        g_RichPresence->Shutdown();
        delete g_RichPresence;
        g_RichPresence = nullptr;
    }

    if (g_Discord) {
        g_Discord->Disconnect();
        delete g_Discord;
        g_Discord = nullptr;
    }

    APIDefs->QuickAccess_Remove(QA_ID);
    APIDefs->GUI_Deregister(AddonOptions);
    APIDefs->GUI_Deregister(AddonRender);

    APIDefs = nullptr;
}

// --- Main Render ---

void AddonRender() {
    if (!g_Discord) return;

    if (g_RichPresence) {
        RPGameState rpState;
        rpState.inGame     = g_MumbleLink && g_MumbleLink->UITick > 0
                             && g_MumbleIdentity.Name[0] != '\0';
        rpState.charName   = g_MumbleIdentity.Name;
        rpState.mapId      = g_MumbleIdentity.MapID;
        rpState.profession = static_cast<unsigned>(g_MumbleIdentity.Profession);
        g_RichPresence->Update(rpState, g_RPConfig);
    }

    // Flush Discord log queue to Nexus log (throttled to every 30 frames)
    static int s_logFrameCount = 0;
    if (++s_logFrameCount >= 30) {
        s_logFrameCount = 0;
        auto logs = g_Discord->DrainLogQueue();
        if (!logs.empty()) {
            for (const auto& msg : logs) {
                APIDefs->Log(LOGL_INFO, "Discordant", msg.c_str());
            }
        }
    }

    // Save token when we first get one
    static std::string s_lastSavedToken;
    std::string currentToken = g_Discord->GetAccessToken();
    if (!currentToken.empty() && currentToken != s_lastSavedToken) {
        SaveConfig();
        s_lastSavedToken = currentToken;
    }

    if (g_Discord->GetState() == DiscordState::Connected) {
        RenderVoiceOverlay();
    }
}

// --- Options Panel ---

void AddonOptions() {
    if (!g_Discord) return;
    ImGui::Text("Discordant - Discord Voice Overlay");
    if (ImGui::SmallButton("Homepage")) {
        ShellExecuteA(NULL, "open", "https://pie.rocks.cc/", NULL, NULL, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Buy me a coffee!")) {
        ShellExecuteA(NULL, "open", "https://ko-fi.com/pieorcake", NULL, NULL, SW_SHOWNORMAL);
    }
    ImGui::Spacing();

    DiscordState state = g_Discord->GetState();
    ImVec4 statusColor(0.98f, 0.66f, 0.07f, 1.0f);
    if (state == DiscordState::Connected)    statusColor = ImVec4(0.24f, 0.78f, 0.44f, 1.0f);
    else if (state == DiscordState::Disconnected) statusColor = ImVec4(0.93f, 0.26f, 0.26f, 1.0f);
    ImGui::TextColored(statusColor, "Status: %s", g_Discord->GetStatusText().c_str());

    ImGui::Spacing();
    if (ImGui::Checkbox("Show user avatars", &g_ShowAvatars)) SaveConfig();
    if (ImGui::Checkbox("Show channel name", &g_ShowChannelName)) SaveConfig();
    if (ImGui::Checkbox("Lock overlay position", &g_LockPosition)) SaveConfig();
    if (ImGui::Checkbox("Hide overlay on mouseover (5s)", &g_HideOnHover)) SaveConfig();
    ImGui::Text("List grow direction");
    if (ImGui::RadioButton("Downward", !g_GrowUpward)) { g_GrowUpward = false; SaveConfig(); }
    ImGui::SameLine();
    if (ImGui::RadioButton("Upward", g_GrowUpward)) { g_GrowUpward = true; SaveConfig(); }
    ImGui::Spacing();
    ImGui::Text("Overlay transparency");
    int alphaPct = (int)(g_OverlayAlpha * 100.0f + 0.5f);
    if (ImGui::SliderInt("##alpha", &alphaPct, 10, 100, "%d%%")) {
        g_OverlayAlpha = alphaPct / 100.0f;
        SaveConfig();
    }
    ImGui::Text("Font scale");
    int scalePct = (int)(g_OverlayScale * 100.0f + 0.5f);
    if (ImGui::SliderInt("##scale", &scalePct, 50, 200, "%d%%")) {
        g_OverlayScale = scalePct / 100.0f;
        SaveConfig();
    }
    ImGui::Text("Colours");
    if (ImGui::ColorEdit4("Speaking##col", g_ColSpeaking, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) SaveConfig();
    if (ImGui::ColorEdit4("Silent##col", g_ColSilent, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) SaveConfig();
    if (ImGui::ColorEdit4("Muted##col", g_ColMuted, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) SaveConfig();
    if (ImGui::ColorEdit4("Channel name##col", g_ColChannel, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) SaveConfig();

    ImGui::Separator();
    ImGui::Text("Rich Presence");
    ImGui::Spacing();

    if (ImGui::Checkbox("Enable Rich Presence", &g_RPConfig.enabled)) SaveConfig();

    if (g_RPConfig.enabled) {
        ImGui::Indent();

        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::InputText("Application ID", g_RPAppIdOverride, sizeof(g_RPAppIdOverride)))
            SaveConfig();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Custom application ID support is reserved for a future update.");

        if (ImGui::Checkbox("Show character name", &g_RPConfig.showCharName)) SaveConfig();
        if (ImGui::Checkbox("Show current map",    &g_RPConfig.showMap))      SaveConfig();
        if (ImGui::Checkbox("Show profession",     &g_RPConfig.showProfession)) SaveConfig();

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
        ImGui::Checkbox("Show party size (not yet available)", &g_RPConfig.showPartySize);
        ImGui::PopStyleVar();

        ImGui::Unindent();
    }

    ImGui::Spacing();
    ImGui::Text("The voice overlay appears automatically when in a Discord voice channel.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (state == DiscordState::Disconnected) {
        if (ImGui::Button("Connect to Discord")) {
            g_Discord->Connect();
        }
    } else {
        if (ImGui::Button("Disconnect from Discord")) {
            g_Discord->Disconnect();
        }
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
    AddonDef.Description = "Discord voice channel overlay for Guild Wars 2";
    AddonDef.Load = AddonLoad;
    AddonDef.Unload = AddonUnload;
    AddonDef.Flags = AF_None;
    AddonDef.Provider = UP_None;
    AddonDef.UpdateLink = "";

    return &AddonDef;
}
