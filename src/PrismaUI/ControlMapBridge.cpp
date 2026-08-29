#include "ControlMapBridge.h"

#include <cstdio>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

#include "Communication.h"
#include "Core.h"

namespace PrismaUI::ControlMapBridge {
    namespace {
        std::mutex g_pendingMutex;
        std::set<Core::PrismaViewId> g_pending;  // Views awaiting a control-map push. Drained per frame.

        // Appends a JSON-quoted, escaped string. Context and device labels are fixed ASCII, but event IDs come
        // from the control map (including mod-registered ones), so escaping is required for valid JSON.
        void AppendJsonString(std::string& out, const char* s) {
            out += '"';
            if (s) {
                for (const char* p = s; *p; ++p) {
                    const unsigned char c = static_cast<unsigned char>(*p);
                    const char* escape = c == '"'    ? "\\\""
                                         : c == '\\' ? "\\\\"
                                         : c == '\b' ? "\\b"
                                         : c == '\f' ? "\\f"
                                         : c == '\n' ? "\\n"
                                         : c == '\r' ? "\\r"
                                         : c == '\t' ? "\\t"
                                                     : nullptr;
                    if (escape) {
                        out += escape;
                    } else if (c < 0x20) {
                        char buf[7];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
                }
            }
            out += '"';
        }

        // JSON key for a standard input device slot
        std::string GetDeviceKey(std::size_t index) {
            return index == 0 ? "keyboard" : index == 1 ? "mouse" : index == 2 ? "gamepad" : "unknown";
        }

        // Canonical name for an input context index
        const char* GetContextName(std::size_t index) {
            return index == 0    ? "Gameplay"
                   : index == 1  ? "MenuMode"
                   : index == 2  ? "Console"
                   : index == 3  ? "ItemMenu"
                   : index == 4  ? "Inventory"
                   : index == 5  ? "DebugText"
                   : index == 6  ? "Favorites"
                   : index == 7  ? "Map"
                   : index == 8  ? "Stats"
                   : index == 9  ? "Cursor"
                   : index == 10 ? "Book"
                   : index == 11 ? "DebugOverlay"
                   : index == 12 ? "Journal"
                   : index == 13 ? "TFCMode"
                   : index == 14 ? "MapDebug"
                   : index == 15 ? "Lockpicking"
                   : index == 16 ? "Favor/Marketplace"  // Favor on 1.5, Marketplace on 1.6
                   : index == 17 ? "Favor"
                                 : "Unknown";
        }

        std::string BuildJson() {
            auto* cm = RE::ControlMap::GetSingleton();
            if (!cm) {
                return "null";
            }

            std::string out;
            out.reserve(16384);
            out += "{\"gamePadType\":";
            out += std::to_string(static_cast<int>(cm->GetGamePadType()));
            out += ",\"contexts\":[";

            // Bound device iteration to the running runtime's device count, and iterate the contexts that
            // exist on every runtime. INPUT_CONTEXT_IDS::kTotal (17) is valid across 1.5 and 1.6.
            // But 1.6 has extra kMarketplace value. See GetContextName.
            const std::size_t deviceCount = RE::ControlMap::InputContext::GetNumDeviceMappings();
            constexpr std::size_t contextCount = RE::UserEvents::INPUT_CONTEXT_IDS::kTotal;

            bool firstContext = true;
            for (std::size_t ctx = 0; ctx < contextCount; ++ctx) {
                RE::ControlMap::InputContext* inputContext = cm->controlMap[ctx];
                if (!inputContext) {
                    continue;
                }

                if (!firstContext) {
                    out += ',';
                }
                firstContext = false;

                out += "{\"index\":";
                out += std::to_string(ctx);
                out += ",\"name\":";
                AppendJsonString(out, GetContextName(ctx));
                out += ",\"devices\":{";

                // Emits one device slot as `"<key>":[<mappings>]` into `out`.
                const auto appendDevice = [&](const std::string& key, std::size_t dev) {
                    AppendJsonString(out, key.c_str());
                    out += ":[";
                    const auto& mappings = inputContext->deviceMappings[dev];
                    for (std::uint32_t i = 0; i < mappings.size(); ++i) {
                        const auto& mapping = mappings[i];
                        if (i != 0) {
                            out += ',';
                        }
                        out += "{\"eventID\":";
                        AppendJsonString(out, mapping.eventID.c_str());
                        out += ",\"inputKey\":";
                        out += std::to_string(mapping.inputKey);
                        out += ",\"modifier\":";
                        out += std::to_string(mapping.modifier);
                        out += ",\"remappable\":";
                        out += mapping.remappable ? "true" : "false";
                        out += ",\"linked\":";
                        out += mapping.linked ? "true" : "false";
                        out += '}';
                    }
                    out += ']';
                };

                // keyboard (0), mouse (1), gamepad (2) are first.
                // The rest are saved in `devices.others`.
                bool anyStandard = false;
                for (std::size_t dev = 0; dev < deviceCount && dev < 3; ++dev) {
                    if (anyStandard) {
                        out += ',';
                    }
                    anyStandard = true;
                    appendDevice(GetDeviceKey(dev), dev);
                }

                if (anyStandard) {
                    out += ',';
                }
                out += "\"others\":{";
                bool firstOther = true;
                for (std::size_t dev = 3; dev < deviceCount; ++dev) {
                    if (!firstOther) {
                        out += ',';
                    }
                    firstOther = false;
                    // Below, JSON requires string keys, but JavaScript will still coerce integers into strings when
                    // used like devices.others[3].
                    appendDevice(std::to_string(dev), dev);
                }
                out += "}}}";  // close `others`, `devices`, and `context`
            }

            out += "]}";
            return out;
        }
    }

    void RequestRefresh(Core::PrismaViewId viewId) {
        std::lock_guard lock(g_pendingMutex);
        g_pending.insert(viewId);
    }

    void ProcessPendingRefreshes() {
        // Check for pending refreshes.
        std::vector<Core::PrismaViewId> pending;
        {
            std::lock_guard lock(g_pendingMutex);
            if (g_pending.empty()) {
                return;
            }
            pending.assign(g_pending.begin(), g_pending.end());
            g_pending.clear();
        }

        // Read ControlMap here on the render thread.
        const std::string json = BuildJson();

        std::string script;
        script.reserve(json.size() + 256);
        script += "(function(c){c.map=";
        script += json;
        script += ";c.dispatchEvent(new CustomEvent(\"refreshcomplete\",{detail:c.map}));})(";
        script += Communication::PrismaControlsEnsureExpression();
        script += ");";

        // Send the JavaScript string to each view via Invoke.
        for (const auto viewId : pending) {
            Communication::Invoke(viewId, script.c_str());
        }
    }
}
