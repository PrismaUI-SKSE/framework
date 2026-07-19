#include "ImeHelper.h"

#include <cstdio>
#include <imm.h>
#pragma comment(lib, "imm32.lib")

#include "Communication.h"
#include "Core.h"
#include "ViewManager.h"

namespace PrismaUI {

namespace {

struct ImeCandidateState {
    std::vector<std::string> candidates;
    int selectedIndex = -1;
};

struct ImeUiState {
    bool active = false;
    std::string composition;
    int caret = 0;
    ImeCandidateState candidateState;
};

UINT GetImeAssociationMessageId() {
    static const UINT kMessageId = RegisterWindowMessageW(L"PrismaUI.ImeAssociation");
    return kMessageId;
}

std::string EscapeForJson(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"')
            r += "\\\"";
        else if (c == '\\')
            r += "\\\\";
        else if (c == '\n')
            r += "\\n";
        else if (c == '\r')
            r += "\\r";
        else if (c == '\t')
            r += "\\t";
        else if (c < 32) {
            char hex[8];
            snprintf(hex, sizeof(hex), "\\u%04x", c);
            r += hex;
        } else
            r += c;
    }
    return r;
}

}  // namespace

const char* ImeHelper::MessageName(UINT uMsg) {
    switch (uMsg) {
        case WM_GETDLGCODE: return "WM_GETDLGCODE";
        case WM_INPUTLANGCHANGE: return "WM_INPUTLANGCHANGE";
        case WM_IME_SETCONTEXT: return "WM_IME_SETCONTEXT";
        case WM_IME_STARTCOMPOSITION: return "WM_IME_STARTCOMPOSITION";
        case WM_IME_COMPOSITION: return "WM_IME_COMPOSITION";
        case WM_IME_ENDCOMPOSITION: return "WM_IME_ENDCOMPOSITION";
        case WM_IME_CHAR: return "WM_IME_CHAR";
        case WM_IME_NOTIFY: return "WM_IME_NOTIFY";
        default: return "WM_IME_?";
    }
}

void ImeHelper::SetCallbacks(ImeEscapeForJSCallback escapeForJS,
                             ImeQueueCommittedCharCallback queueCommittedChar,
                             ImeConvertUtf16ToUtf8Callback convertUtf16ToUtf8) {
    m_escapeForJS = std::move(escapeForJS);
    m_queueCommittedChar = std::move(queueCommittedChar);
    m_convertUtf16ToUtf8 = std::move(convertUtf16ToUtf8);
}

void ImeHelper::SetContext(const ImeHelperContext& ctx) { m_ctx = ctx; }

void ImeHelper::SetExecutor(SingleThreadExecutor* executor) { m_executor = executor; }

void ImeHelper::Initialize(HWND hwnd) {
    if (!hwnd) return;

    // Use system's IME context to preserve conversion mode across focus cycles.
    // ImmCreateContext yields a fresh context that loses Japanese IME state (a->あ, etc.) on re-associate.
    m_context = ImmGetContext(hwnd);
    if (m_context) {
        m_contextOwned = false;
    } else {
        m_context = ImmCreateContext();
        m_contextOwned = true;
        if (!m_context) {
            logger::warn("IME: Failed to create IME context");
        }
    }

    // Start with IME disassociated. The active text input state drives
    // association via posted window-thread messages.
    m_associated = false;
}

bool ImeHelper::IsTextInputFocused() const {
    return m_ctx.isTextInputFocused && m_ctx.isTextInputFocused->load();
}

void ImeHelper::DispatchScriptToView(Core::PrismaViewId viewId, const std::string& script) {
    if (!viewId || script.empty()) {
        return;
    }

    if (m_executor && m_executor->IsWorkerThread() && m_ctx.viewsMap && m_ctx.viewsMapMutex) {
        std::shared_ptr<Core::PrismaView> viewData = nullptr;
        {
            std::shared_lock lock(*m_ctx.viewsMapMutex);
            auto it = m_ctx.viewsMap->find(viewId);
            if (it != m_ctx.viewsMap->end()) {
                viewData = it->second;
            }
        }

        if (!viewData || !viewData->ultralightView) {
            return;
        }

        try {
            viewData->ultralightView->EvaluateScript(ultralight::String(script.c_str()), nullptr);
        } catch (const std::exception& e) {
            logger::error("IME: Failed to dispatch state to View [{}]: {}", viewId, e.what());
        } catch (...) {
            logger::error("IME: Failed to dispatch state to View [{}]: unknown exception", viewId);
        }
        return;
    }

    Communication::Invoke(viewId, script.c_str());
}

void ImeHelper::Shutdown(HWND hwnd) {
    if (!m_context) return;

    m_lastKnownTextInputFocus = false;
    m_associated = false;
    if (hwnd) {
        ImmAssociateContext(hwnd, nullptr);
    }
    if (m_contextOwned) {
        ImmDestroyContext(m_context);
    } else if (hwnd) {
        ImmReleaseContext(hwnd, m_context);
    }
    m_context = nullptr;
}

void ImeHelper::SetAssociation(bool enabled) {
    if (!m_context || !m_ctx.hwnd) return;

    const bool previous = m_associated.load();
    if (previous == enabled) return;

    HWND hwnd = m_ctx.hwnd;
    HIMC himc = enabled ? m_context : nullptr;
    if (!PostMessage(hwnd, GetImeAssociationMessageId(), enabled ? 1 : 0, reinterpret_cast<LPARAM>(himc))) {
        logger::warn("IME: Failed to post association change (enabled={})", enabled);
        return;
    }

    m_associated = enabled;
}

bool ImeHelper::HandleControlMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* outResult) {
    if (uMsg != GetImeAssociationMessageId()) {
        return false;
    }

    HIMC himc = wParam ? reinterpret_cast<HIMC>(lParam) : nullptr;
    ImmAssociateContext(hwnd, himc);
    if (outResult) {
        *outResult = 0;
    }
    return true;
}

void ImeHelper::ModifySetContextLParam(LPARAM* lParam, UINT uMsg) {
    if (uMsg == WM_IME_SETCONTEXT && lParam) {
        // Suppress Windows fallback IME UI in fullscreen (candidate/composition windows).
        // Skyrim runs fullscreen; native IME windows would overlay the game.
        *lParam = 0;
    }
}

void ImeHelper::ClearStateInJS(Core::PrismaViewId viewId) {
    SendStateToJS(viewId, nullptr, false);
}

void ImeHelper::SendStateToJS(Core::PrismaViewId viewId, HWND hwnd, bool active) {
    if (!viewId) return;
    if (!m_escapeForJS || !m_convertUtf16ToUtf8) return;

    ImeUiState state;
    state.active = active;

    if (hwnd) {
        HIMC himc = ImmGetContext(hwnd);
        if (himc) {
            // Read composition string
            const LONG compSize = ImmGetCompositionString(himc, GCS_COMPSTR, nullptr, 0);
            if (compSize > 0) {
                std::vector<wchar_t> buffer((compSize / static_cast<LONG>(sizeof(wchar_t))) + 1, L'\0');
                const LONG copied = ImmGetCompositionString(himc, GCS_COMPSTR, buffer.data(), compSize);
                if (copied > 0) {
                    std::wstring wideComp(buffer.data(), copied / static_cast<LONG>(sizeof(wchar_t)));
                    state.composition = m_convertUtf16ToUtf8(wideComp.data(), static_cast<int>(wideComp.size()));
                }
            }

            // Read caret position
            const LONG cursorPos = ImmGetCompositionString(himc, GCS_CURSORPOS, nullptr, 0);
            if (cursorPos >= 0) {
                state.caret = static_cast<int>(cursorPos);
            }

            // Read candidate list
            const DWORD bytesNeeded = ImmGetCandidateList(himc, 0, nullptr, 0);
            if (bytesNeeded > 0) {
                std::vector<char> buffer(bytesNeeded);
                const DWORD bytesCopied =
                    ImmGetCandidateList(himc, 0, reinterpret_cast<LPCANDIDATELIST>(buffer.data()), bytesNeeded);
                if (bytesCopied >= sizeof(CANDIDATELIST)) {
                    auto* candidateList = reinterpret_cast<LPCANDIDATELIST>(buffer.data());
                    const DWORD candidateCount = candidateList->dwCount;
                    if (candidateCount > 0) {
                        const DWORD pageStart =
                            candidateList->dwPageStart < candidateCount ? candidateList->dwPageStart : 0;
                        const DWORD pageSize =
                            candidateList->dwPageSize == 0 ? candidateCount : candidateList->dwPageSize;
                        const DWORD pageEnd =
                            (pageStart + pageSize) < candidateCount ? (pageStart + pageSize) : candidateCount;

                        for (DWORD index = pageStart; index < pageEnd; ++index) {
                            if (candidateList->dwOffset[index] >= bytesCopied) continue;
                            const wchar_t* candidate =
                                reinterpret_cast<const wchar_t*>(buffer.data() + candidateList->dwOffset[index]);
                            std::wstring wideCandidate(candidate ? candidate : L"");
                            std::string utf8Candidate =
                                wideCandidate.empty() ? ""
                                                    : m_convertUtf16ToUtf8(wideCandidate.data(),
                                                                           static_cast<int>(wideCandidate.size()));
                            state.candidateState.candidates.push_back(std::move(utf8Candidate));
                        }

                        if (candidateList->dwSelection >= pageStart && candidateList->dwSelection < pageEnd) {
                            state.candidateState.selectedIndex =
                                static_cast<int>(candidateList->dwSelection - pageStart);
                        }

                        // Override with recommended conversion for Japanese IME (とうきょう -> 東京)
                        if (state.candidateState.candidates.size() > 0 && !state.composition.empty()) {
                            const HKL hkl =
                                GetKeyboardLayout(GetWindowThreadProcessId(hwnd, nullptr));
                            if (hkl) {
                                std::wstring wideComp;
                                const LONG cs = ImmGetCompositionString(himc, GCS_COMPSTR, nullptr, 0);
                                if (cs > 0) {
                                    std::vector<wchar_t> compBuf((cs / sizeof(wchar_t)) + 1, L'\0');
                                    ImmGetCompositionString(himc, GCS_COMPSTR, compBuf.data(), cs);
                                    wideComp.assign(compBuf.data(), cs / sizeof(wchar_t));
                                }
                                if (!wideComp.empty()) {
                                    const DWORD bufSize =
                                        ImmGetConversionListW(hkl, himc, wideComp.c_str(), nullptr, 0, GCL_CONVERSION);
                                    if (bufSize >= sizeof(CANDIDATELIST)) {
                                        std::vector<char> convBuf(bufSize);
                                        if (ImmGetConversionListW(hkl, himc, wideComp.c_str(),
                                                                 reinterpret_cast<LPCANDIDATELIST>(convBuf.data()),
                                                                 bufSize, GCL_CONVERSION) != 0) {
                                            auto* convList = reinterpret_cast<LPCANDIDATELIST>(convBuf.data());
                                            if (convList->dwCount > 0 &&
                                                !(convList->dwStyle == IME_CAND_CODE && convList->dwCount == 1)) {
                                                const DWORD offset0 = convList->dwOffset[0];
                                                if (offset0 < bufSize) {
                                                    const wchar_t* str =
                                                        reinterpret_cast<const wchar_t*>(convBuf.data() + offset0);
                                                    if (str) {
                                                        std::wstring recommended(str);
                                                        std::string recommendedUtf8 =
                                                            m_convertUtf16ToUtf8(recommended.data(),
                                                                               static_cast<int>(recommended.size()));
                                                        for (size_t i = 0; i < state.candidateState.candidates.size();
                                                             ++i) {
                                                            if (state.candidateState.candidates[i] == recommendedUtf8) {
                                                                state.candidateState.selectedIndex =
                                                                    static_cast<int>(i);
                                                                break;
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            ImmReleaseContext(hwnd, himc);
        } else if (!active) {
            // No context, clear state
        }
    }

    // Build and dispatch JSON
    std::string json = "{";
    json += "\"active\":";
    json += state.active ? "true" : "false";
    json += ",\"composition\":\"";
    json += EscapeForJson(state.composition);
    json += "\",\"caret\":";
    json += std::to_string(state.caret);
    json += ",\"selectedIndex\":";
    json += std::to_string(state.candidateState.selectedIndex);
    json += ",\"candidates\":[";
    for (size_t i = 0; i < state.candidateState.candidates.size(); ++i) {
        if (i > 0) json += ",";
        json += "\"";
        json += EscapeForJson(state.candidateState.candidates[i]);
        json += "\"";
    }
    json += "]}";

    const std::string escapedJson = m_escapeForJS(json);
    if (escapedJson.empty()) return;

    const std::string script =
        "window.dispatchEvent(new CustomEvent('prismaIME_state',{detail:JSON.parse('" + escapedJson + "')}))";
    DispatchScriptToView(viewId, script);
}

bool ImeHelper::HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                              Core::PrismaViewId focusedViewId, bool* outHandled) {
    if (!outHandled || focusedViewId == 0) return false;

    if (!m_associated.load()) return false;
    if (!IsTextInputFocused()) return false;

    switch (uMsg) {
        case WM_IME_STARTCOMPOSITION:
            *outHandled = true;
            SendStateToJS(focusedViewId, hwnd, true);
            return true;
        case WM_IME_ENDCOMPOSITION:
            *outHandled = true;
            ClearStateInJS(focusedViewId);
            return true;
        case WM_IME_CHAR:
            *outHandled = true;
            return true;
        case WM_IME_COMPOSITION: {
            *outHandled = true;
            if (m_queueCommittedChar) {
                HIMC himc = ImmGetContext(hwnd);
                if (himc) {
                    if (lParam & GCS_RESULTSTR) {
                        const int size = ImmGetCompositionString(himc, GCS_RESULTSTR, nullptr, 0);
                        if (size > 0) {
                            std::vector<wchar_t> buf((size / sizeof(wchar_t)) + 1, L'\0');
                            ImmGetCompositionString(himc, GCS_RESULTSTR, buf.data(),
                                                   static_cast<DWORD>(buf.size() * sizeof(wchar_t)));
                            m_queueCommittedChar(std::wstring(buf.data()), lParam);
                        }
                    }
                    ImmReleaseContext(hwnd, himc);
                }
            }
            SendStateToJS(focusedViewId, hwnd, true);
            return true;
        }
        case WM_IME_NOTIFY:
            if (wParam == IMN_CHANGECANDIDATE || wParam == IMN_OPENCANDIDATE || wParam == IMN_CLOSECANDIDATE) {
                *outHandled = true;
                SendStateToJS(focusedViewId, hwnd, true);
            }
            return *outHandled;
        default:
            return false;
    }
}

void ImeHelper::UpdateStateImpl(Core::PrismaViewId viewId) {
    if (!m_ctx.viewsMap || !m_ctx.viewsMapMutex || viewId == 0) {
        m_lastKnownTextInputFocus = false;
        return;
    }

    bool stillFocused = false;
    if (m_ctx.focusedViewIdMutex && m_ctx.currentlyFocusedViewId && m_ctx.isAnyInputCaptureActive) {
        std::lock_guard lock(*m_ctx.focusedViewIdMutex);
        stillFocused =
            m_ctx.isAnyInputCaptureActive->load() && *m_ctx.currentlyFocusedViewId == viewId;
    }

    if (!stillFocused) {
        m_lastKnownTextInputFocus = false;
        ClearStateInJS(viewId);
        return;
    }

    const bool textInputFocused = IsTextInputFocused();
    m_lastKnownTextInputFocus = textInputFocused;

    if (!textInputFocused) {
        ClearStateInJS(viewId);
    }
}

void ImeHelper::UpdateStateForFocusedView(Core::PrismaViewId viewId) {
    if (!m_executor) return;

    if (m_executor->IsWorkerThread()) {
        UpdateStateImpl(viewId);
        return;
    }

    m_executor->submit_with_priority(
        SingleThreadExecutor::Priority::HIGH,
        [this, viewId]() { UpdateStateImpl(viewId); });
}

}  // namespace PrismaUI
