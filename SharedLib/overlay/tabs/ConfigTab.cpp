// ConfigTab.cpp — overlay "Config" tab: raw config.yaml editor (Save/Reload/Apply).
//
// Layer: game (Layer 3). Registered via detail::RegisterConfigTab() (called from
// OverlayConsole::Init in display order). Shared services come from OverlayTabs.h.

#include "OverlayTabs.h"
#include "Lib_NetLogConfig.h"   // NetLogConfig::ConfigPath

#include <imgui.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>

namespace OverlayConsole
{
    namespace
    {
        using detail::RunCommand;

        struct ConfigTab : detail::OverlayTab<ConfigTab>
        {
            static constexpr const char* kName = "Config";

            // ImGui renders '\t' as a fixed 4-space advance; expanding to this many spaces
            // keeps indentation visually identical.
            static constexpr int kTabWidth = 4;

            char        text[32768] = {};
            bool        loaded = false;
            std::string status;

            // YAML forbids tabs for indentation, but the editor allows typing/pasting them
            // (ImGuiInputTextFlags_AllowTabInput). Expand each '\t' so Save can't write an
            // unparseable file (also catches pastes).
            static std::string ExpandTabs(const char* s)
            {
                std::string out;
                for (const char* p = s; *p; ++p)
                {
                    if (*p == '\t') out.append(kTabWidth, ' ');
                    else            out.push_back(*p);
                }
                return out;
            }

            void Load()
            {
                loaded = true;
                text[0] = '\0';
                const std::string path = NetLogConfig::ConfigPath();
                if (path.empty()) { status = "config path unresolved"; return; }
                std::ifstream f(path, std::ios::binary);
                if (!f) { status = "not found (Save creates it): " + path; return; }
                std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                if (body.size() >= sizeof(text)) { status = "file too large to edit here"; return; }
                std::copy(body.begin(), body.end(), text);
                text[body.size()] = '\0';
                status = "loaded " + path;
            }

            void Save()
            {
                const std::string path = NetLogConfig::ConfigPath();
                if (path.empty()) { status = "config path unresolved"; return; }
                const std::string body = ExpandTabs(text);   // YAML safety: no literal tabs
                std::ofstream f(path, std::ios::binary | std::ios::trunc);
                if (!f) { status = "save FAILED: " + path; return; }
                f << body;
                status = "saved " + path;
                // Reflect the normalized text back into the editor so the buffer matches disk.
                if (body.size() < sizeof(text))
                {
                    std::copy(body.begin(), body.end(), text);
                    text[body.size()] = '\0';
                }
            }

            void Draw()
            {
                if (!loaded) Load();

                if (ImGui::Button("Save"))   Save();
                ImGui::SameLine();
                if (ImGui::Button("Reload")) Load();
                ImGui::SameLine();
                if (ImGui::Button("Apply in-game"))   // re-read skip lists + re-run autorun
                {
                    RunCommand("reloadnetlog");
                    RunCommand("runcfg");
                    status = "applied (reloadnetlog + runcfg) — Save first to persist";
                }
                ImGui::SameLine();
                ImGui::TextDisabled("edits config.yaml on disk (persists)");

                if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());

                const float foot = ImGui::GetFrameHeightWithSpacing();
                ImGui::InputTextMultiline("##cfg", text, sizeof(text),
                                          ImVec2(-1.f, -foot), ImGuiInputTextFlags_AllowTabInput);
                ImGui::TextDisabled("Save writes to disk; Apply reloads it in-game. Full reload still needed for some settings.");
            }
        };
    } // anonymous namespace

    void detail::RegisterConfigTab() { static ConfigTab s; (void)s; }
}
