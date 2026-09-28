// Builds RetroArch playlists from the ROM folders each time RetroArch is opened,
// so dropped-in games appear under Playlists already paired with a core.
#include "apps.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace {

struct System {
    const wchar_t* folder;     // apps\roms\<folder>
    const char* playlist;      // RetroArch database name (also enables thumbnails)
    const wchar_t* core;       // cores\<core>_libretro.dll
    const char* coreName;
    std::set<std::wstring> extensions;
};

const std::set<std::wstring> DISC = {L".m3u", L".cue", L".chd", L".ccd", L".iso", L".img", L".pbp", L".bin"};
std::set<std::wstring> ext(std::initializer_list<const wchar_t*> list, bool disc = false) {
    std::set<std::wstring> s(list.begin(), list.end());
    if (disc) s.insert(DISC.begin(), DISC.end());
    return s;
}

// Keep in sync with SYSTEMS in tools/setup_apps.py and docs/ROMS.md.
const System SYSTEMS[] = {
    {L"atari2600", "Atari - 2600", L"stella", "Atari - 2600 (Stella)", ext({L".a26", L".bin", L".zip"})},
    {L"atari7800", "Atari - 7800", L"prosystem", "Atari - 7800 (ProSystem)", ext({L".a78", L".bin", L".zip"})},
    {L"lynx", "Atari - Lynx", L"handy", "Atari - Lynx (Handy)", ext({L".lnx", L".zip"})},
    {L"nes", "Nintendo - Nintendo Entertainment System", L"nestopia", "Nintendo - NES / Famicom (Nestopia)", ext({L".nes", L".fds", L".unf", L".unif", L".zip", L".7z"})},
    {L"snes", "Nintendo - Super Nintendo Entertainment System", L"snes9x", "Nintendo - SNES / SFC (Snes9x - Current)", ext({L".sfc", L".smc", L".fig", L".swc", L".bs", L".zip", L".7z"})},
    {L"n64", "Nintendo - Nintendo 64", L"mupen64plus_next", "Nintendo - Nintendo 64 (Mupen64Plus-Next)", ext({L".n64", L".v64", L".z64", L".zip", L".7z"})},
    {L"gb", "Nintendo - Game Boy", L"gambatte", "Nintendo - Game Boy / Color (Gambatte)", ext({L".gb", L".gbc", L".dmg", L".zip", L".7z"})},
    {L"gba", "Nintendo - Game Boy Advance", L"mgba", "Nintendo - Game Boy Advance (mGBA)", ext({L".gba", L".zip", L".7z"})},
    {L"nds", "Nintendo - Nintendo DS", L"melondsds", "Nintendo - DS (melonDS DS)", ext({L".nds", L".zip", L".7z"})},
    {L"3ds", "Nintendo - Nintendo 3DS", L"citra", "Nintendo - 3DS (Citra)", ext({L".3ds", L".3dsx", L".cci", L".cxi", L".app", L".elf"})},
    {L"gamecube", "Nintendo - GameCube", L"dolphin", "Nintendo - GameCube / Wii (Dolphin)", ext({L".iso", L".gcm", L".gcz", L".rvz", L".ciso", L".m3u"})},
    {L"wii", "Nintendo - Wii", L"dolphin", "Nintendo - GameCube / Wii (Dolphin)", ext({L".iso", L".wbfs", L".rvz", L".gcz", L".wad", L".m3u"})},
    {L"virtualboy", "Nintendo - Virtual Boy", L"mednafen_vb", "Nintendo - Virtual Boy (Beetle VB)", ext({L".vb", L".vboy", L".zip"})},
    {L"mastersystem", "Sega - Master System - Mark III", L"genesis_plus_gx", "Sega - MS/GG/MD/CD (Genesis Plus GX)", ext({L".sms", L".zip", L".7z"})},
    {L"gamegear", "Sega - Game Gear", L"genesis_plus_gx", "Sega - MS/GG/MD/CD (Genesis Plus GX)", ext({L".gg", L".zip", L".7z"})},
    {L"genesis", "Sega - Mega Drive - Genesis", L"genesis_plus_gx", "Sega - MS/GG/MD/CD (Genesis Plus GX)", ext({L".md", L".gen", L".smd", L".bin", L".68k", L".zip", L".7z"})},
    {L"segacd", "Sega - Mega-CD - Sega CD", L"genesis_plus_gx", "Sega - MS/GG/MD/CD (Genesis Plus GX)", ext({}, true)},
    {L"32x", "Sega - 32X", L"picodrive", "Sega - MS/MD/CD/32X (PicoDrive)", ext({L".32x", L".zip", L".7z"})},
    {L"saturn", "Sega - Saturn", L"mednafen_saturn", "Sega - Saturn (Beetle Saturn)", ext({}, true)},
    {L"dreamcast", "Sega - Dreamcast", L"flycast", "Sega - Dreamcast/NAOMI (Flycast)", ext({L".gdi", L".cdi", L".chd", L".cue", L".m3u"})},
    {L"psx", "Sony - PlayStation", L"pcsx_rearmed", "Sony - PlayStation (PCSX ReARMed)", ext({}, true)},
    {L"ps2", "Sony - PlayStation 2", L"pcsx2", "Sony - PlayStation 2 (LRPS2)", ext({L".iso", L".chd", L".cso", L".bin", L".cue", L".m3u"})},
    {L"psp", "Sony - PlayStation Portable", L"ppsspp", "Sony - PlayStation Portable (PPSSPP)", ext({L".iso", L".cso", L".pbp", L".chd", L".elf"})},
    {L"pcengine", "NEC - PC Engine - TurboGrafx 16", L"mednafen_pce_fast", "NEC - PC Engine / CD (Beetle PCE FAST)", ext({L".pce", L".zip"})},
    {L"pcenginecd", "NEC - PC Engine CD - TurboGrafx-CD", L"mednafen_pce_fast", "NEC - PC Engine / CD (Beetle PCE FAST)", ext({}, true)},
    {L"ngp", "SNK - Neo Geo Pocket Color", L"mednafen_ngp", "SNK - Neo Geo Pocket / Color (Beetle NeoPop)", ext({L".ngp", L".ngc", L".zip"})},
    {L"wonderswan", "Bandai - WonderSwan Color", L"mednafen_wswan", "Bandai - WonderSwan / Color (Beetle Cygne)", ext({L".ws", L".wsc", L".zip"})},
    {L"msx", "Microsoft - MSX", L"bluemsx", "MSX/SVI/ColecoVision/SG-1000 (blueMSX)", ext({L".rom", L".mx1", L".mx2", L".dsk", L".cas", L".zip"})},
    {L"arcade", "FBNeo - Arcade Games", L"fbneo", "Arcade (FinalBurn Neo)", ext({L".zip", L".7z"})},
    {L"mame", "MAME 2003-Plus", L"mame2003_plus", "Arcade (MAME 2003-Plus)", ext({L".zip"})},
    {L"dos", "DOS", L"dosbox_pure", "DOS (DOSBox-Pure)", ext({L".zip", L".dosz", L".exe", L".com", L".bat", L".iso", L".cue"})},
    {L"doom", "DOOM", L"prboom", "DOOM (PrBoom)", ext({L".wad"})},
};

std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string json(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out;
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = wchar_t(towlower(c));
    return s;
}

// Disc images: prefer .m3u over its discs, and .cue over its .bin tracks.
bool skipDiscFile(const fs::path& file, const std::set<fs::path>& folderHasM3u, const std::set<fs::path>& folderHasCue) {
    auto ext = lower(file.extension().wstring());
    bool disc = ext == L".cue" || ext == L".chd" || ext == L".bin" || ext == L".iso" || ext == L".img" || ext == L".pbp";
    if (disc && folderHasM3u.count(file.parent_path())) return true;
    return ext == L".bin" && folderHasCue.count(file.parent_path());
}

}  // namespace

int writePlaylists(const Config& c) {
    std::error_code ec;
    fs::path roms = c.roms, retro = fs::path(c.retroarch).parent_path();
    fs::path out = retro / L"playlists";
    fs::create_directories(out, ec);
    int total = 0;
    for (auto& sys : SYSTEMS) {
        fs::path dir = roms / sys.folder;
        fs::path core = retro / L"cores" / (std::wstring(sys.core) + L"_libretro.dll");
        std::vector<fs::path> games;
        std::set<fs::path> m3u, cue;
        if (fs::is_directory(dir, ec)) {
            for (auto& e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
                if (!e.is_regular_file(ec)) continue;
                auto ext = lower(e.path().extension().wstring());
                if (ext == L".m3u") m3u.insert(e.path().parent_path());
                if (ext == L".cue") cue.insert(e.path().parent_path());
                if (!sys.extensions.count(ext)) continue;
                if (lower(e.path().filename().wstring()) == L"prboom.wad") continue;  // engine data, not a game
                games.push_back(e.path());
            }
        }
        std::sort(games.begin(), games.end());
        std::string corePath = json(utf8(core.wstring())), coreName = json(sys.coreName);
        std::string body = "{\n  \"version\": \"1.5\",\n  \"default_core_path\": \"" + corePath + "\",\n  \"default_core_name\": \"" +
                           coreName + "\",\n  \"label_display_mode\": 0,\n  \"right_thumbnail_mode\": 0,\n  \"left_thumbnail_mode\": 0,\n"
                           "  \"sort_mode\": 0,\n  \"items\": [";
        int count = 0;
        for (auto& g : games) {
            if (skipDiscFile(g, m3u, cue)) continue;
            body += std::string(count ? "," : "") + "\n    {\n      \"path\": \"" + json(utf8(g.wstring())) + "\",\n      \"label\": \"" +
                    json(utf8(g.stem().wstring())) + "\",\n      \"core_path\": \"" + corePath + "\",\n      \"core_name\": \"" + coreName +
                    "\",\n      \"crc32\": \"DETECT\",\n      \"db_name\": \"" + json(sys.playlist) + ".lpl\"\n    }";
            count++;
        }
        body += "\n  ]\n}\n";
        fs::path file = out / (std::string(sys.playlist) + ".lpl");
        if (!count) { fs::remove(file, ec); continue; }  // no empty playlists in the menu
        std::ofstream(file, std::ios::binary) << body;
        total += count;
        hostLog("playlist %s: %d game(s)", sys.playlist, count);
    }
    return total;
}
