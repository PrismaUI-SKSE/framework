#include "ImeHelper.h"

#include <imm.h>

#include <cstdio>
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
            case WM_GETDLGCODE:
                return "WM_GETDLGCODE";
            case WM_INPUTLANGCHANGE:
                return "WM_INPUTLANGCHANGE";
            case WM_IME_SETCONTEXT:
                return "WM_IME_SETCONTEXT";
            case WM_IME_STARTCOMPOSITION:
                return "WM_IME_STARTCOMPOSITION";
            case WM_IME_COMPOSITION:
                return "WM_IME_COMPOSITION";
            case WM_IME_ENDCOMPOSITION:
                return "WM_IME_ENDCOMPOSITION";
            case WM_IME_CHAR:
                return "WM_IME_CHAR";
            case WM_IME_NOTIFY:
                return "WM_IME_NOTIFY";
            default:
                return "WM_IME_?";
        }
    }

    void ImeHelper::SetCallbacks(ImeEscapeForJSCallback escapeForJS, ImeQueueCommittedCharCallback queueCommittedChar,
                                 ImeConvertUtf16ToUtf8Callback convertUtf16ToUtf8) {
        m_escapeForJS = std::move(escapeForJS);
        m_queueCommittedChar = std::move(queueCommittedChar);
        m_convertUtf16ToUtf8 = std::move(convertUtf16ToUtf8);
    }

    void ImeHelper::SetContext(const ImeHelperContext& ctx) { m_ctx = ctx; }

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

    bool ImeHelper::IsTextInputFocused() const { return m_ctx.isTextInputFocused && m_ctx.isTextInputFocused->load(); }

    void ImeHelper::Shutdown(HWND hwnd) {
        if (!m_context) return;

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

    bool ImeHelper::HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, Core::PrismaViewId focusedViewId,
                                  bool* outHandled) {
        if (!outHandled || focusedViewId == 0) return false;

        if (!m_associated.load()) return false;
        if (!IsTextInputFocused()) return false;

        switch (uMsg) {
            case WM_IME_STARTCOMPOSITION:
                *outHandled = true;
                return true;
            case WM_IME_ENDCOMPOSITION:
                *outHandled = true;
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

                return true;
            }
            case WM_IME_NOTIFY:
                if (wParam == IMN_CHANGECANDIDATE || wParam == IMN_OPENCANDIDATE || wParam == IMN_CLOSECANDIDATE) {
                    *outHandled = true;
                }

                return *outHandled;
            default:
                return false;
        }
    }

}  // namespace PrismaUI
