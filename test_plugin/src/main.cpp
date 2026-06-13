#include <chrono>
#include <cstddef>
#include <thread>

#include "PCH.h"
#include "PrismaUI_API.h"

namespace {
    constexpr const char* kViewPath = "prismaui_api_test.html";
    constexpr const char* kEchoViewPaths[] = {"prismaui_echo_view_left.html", "prismaui_echo_view_right.html"};
    constexpr std::uint8_t kRawVersionProbeFirst = 0;
    constexpr std::uint8_t kRawVersionProbeLast = 6;

    struct CallbackState {
        const char* name;
        std::uint32_t marker;
    };

    CallbackState g_domReadyState{"dom-ready", 0x50554901};
    CallbackState g_invokeState{"invoke", 0x50554902};
    CallbackState g_domInvokeState{"dom-invoke", 0x50554903};
    CallbackState g_listenerState{"listener", 0x50554904};
    CallbackState g_consoleState{"console", 0x50554905};
    CallbackState g_controlState{"control", 0x50554906};

    PRISMA_UI_API::IVPrismaUI1* g_apiV1 = nullptr;
    PRISMA_UI_API::IVPrismaUI2* g_apiV2 = nullptr;
    PRISMA_UI_API::IVPrismaUI3* g_apiV3 = nullptr;
    PrismaView g_testView = 0;
    PrismaView g_echoViews[2]{};
    bool g_echoViewsVisible[2]{true, true};
    std::atomic_bool g_startedLifecycleTest = false;
    constexpr const char* kSmokePassMarker = "PRISMAUI_SMOKE_TEST_PASS";
    constexpr unsigned int kSmokeExitWatchdogSeconds = 8;

    std::atomic_bool g_domReadySeen = false;
    std::atomic_bool g_domInvokeSeen = false;
    std::atomic_bool g_listenerSeen = false;
    std::atomic_bool g_consoleSeen = false;
    std::atomic_bool g_smokePassLogged = false;
    std::atomic_bool g_autoExitScheduled = false;

    [[nodiscard]] const char* BoolText(const bool value) noexcept { return value ? "true" : "false"; }

    [[nodiscard]] const char* ConsoleLevelName(const PRISMA_UI_API::ConsoleMessageLevel level) noexcept {
        switch (level) {
            case PRISMA_UI_API::ConsoleMessageLevel::Log:
                return "log";
            case PRISMA_UI_API::ConsoleMessageLevel::Warning:
                return "warning";
            case PRISMA_UI_API::ConsoleMessageLevel::Error:
                return "error";
            case PRISMA_UI_API::ConsoleMessageLevel::Debug:
                return "debug";
            case PRISMA_UI_API::ConsoleMessageLevel::Info:
                return "info";
        }
        return "unknown";
    }

    [[nodiscard]] const char* MessageName(const std::uint32_t type) noexcept {
        switch (type) {
            case SKSE::MessagingInterface::kPostLoad:
                return "PostLoad";
            case SKSE::MessagingInterface::kPostPostLoad:
                return "PostPostLoad";
            case SKSE::MessagingInterface::kInputLoaded:
                return "InputLoaded";
            case SKSE::MessagingInterface::kDataLoaded:
                return "DataLoaded";
            case SKSE::MessagingInterface::kNewGame:
                return "NewGame";
            case SKSE::MessagingInterface::kPreLoadGame:
                return "PreLoadGame";
            case SKSE::MessagingInterface::kPostLoadGame:
                return "PostLoadGame";
            case SKSE::MessagingInterface::kSaveGame:
                return "SaveGame";
            case SKSE::MessagingInterface::kDeleteGame:
                return "DeleteGame";
        }
        return "Unknown";
    }

    [[nodiscard]] bool ContainsText(const char* value, const std::string_view needle) noexcept {
        return value && std::string_view(value).find(needle) != std::string_view::npos;
    }

    [[nodiscard]] bool SmokeAutoExitEnabled() noexcept {
        char value[8]{};
        const DWORD length =
            GetEnvironmentVariableA("PRISMAUI_SMOKE_AUTO_EXIT", value, static_cast<DWORD>(sizeof(value)));
        return length > 0 && value[0] != '0';
    }

    void RunConsoleCommand(const char* command) {
        const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
        auto* script = factory ? factory->Create() : nullptr;
        if (!script) {
            logger::warn("Smoke auto-exit: could not create Script form for console command '{}'", command);
            return;
        }

        logger::info("Smoke auto-exit: running console command '{}'", command);
        script->SetCommand(command);
        script->CompileAndRun(nullptr);
        delete script;
    }

    void RequestCleanQuit() {
        // "qqq" (the QuitGame console command) performs a real quit-to-desktop that the engine honors both
        // in-gameplay and at the Main Menu. Writing RE::Main::quitGame directly is ignored at the Main Menu,
        // so qqq is the clean exit path here.
        RunConsoleCommand("qqq");
    }

    void StartQuitWatchdog() {
        logger::info("Smoke auto-exit: arming hard-termination watchdog ({}s)", kSmokeExitWatchdogSeconds);
        std::thread([]() {
            std::this_thread::sleep_for(std::chrono::seconds(kSmokeExitWatchdogSeconds));
            logger::warn("Smoke auto-exit watchdog: process still alive after {}s; calling TerminateProcess",
                         kSmokeExitWatchdogSeconds);
            ::TerminateProcess(::GetCurrentProcess(), 0);
        }).detach();
    }

    void RequestGameQuitAfterSmokePass() {
        bool expected = false;
        if (!g_autoExitScheduled.compare_exchange_strong(expected, true)) {
            return;
        }

        // Primary exit is the "qqq" console command (a clean quit-to-desktop honored even at the Main Menu).
        // The watchdog is a guaranteed fallback if qqq cannot run or shutdown stalls.
        StartQuitWatchdog();

        auto* taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            logger::warn("Smoke auto-exit: SKSE task interface unavailable; requesting clean quit inline");
            RequestCleanQuit();
            return;
        }

        logger::info("Smoke auto-exit: scheduling clean quit on the SKSE task interface");
        taskInterface->AddTask([]() { RequestCleanQuit(); });
    }

    void CheckSmokeCompletion() {
        if (!g_domReadySeen.load(std::memory_order_acquire) || !g_domInvokeSeen.load(std::memory_order_acquire) ||
            !g_listenerSeen.load(std::memory_order_acquire) || !g_consoleSeen.load(std::memory_order_acquire)) {
            return;
        }

        bool expected = false;
        if (!g_smokePassLogged.compare_exchange_strong(expected, true)) {
            return;
        }

        logger::info("{}: domReady=true, domInvoke=true, listener=true, console=true", kSmokePassMarker);

        if (SmokeAutoExitEnabled()) {
            RequestGameQuitAfterSmokePass();
        }
    }

    [[nodiscard]] CallbackState* AsState(void* state) noexcept { return static_cast<CallbackState*>(state); }

    void LogState(const char* callbackName, void* state) {
        const auto* callbackState = AsState(state);
        if (!callbackState) {
            logger::warn("{} callback received null state", callbackName);
            return;
        }

        logger::info("{} callback state: name={}, marker=0x{:08X}", callbackName, callbackState->name,
                     callbackState->marker);
    }

    void InvokeCallback(const char* result, void* state) {
        LogState("InvokeV2", state);
        logger::info("InvokeV2 result: {}", result ? result : "<null>");
    }

    void DomInvokeCallback(const char* result, void* state) {
        LogState("DOM-ready InvokeV2", state);
        logger::info("DOM-ready InvokeV2 result: {}", result ? result : "<null>");
        if (ContainsText(result, "readyState=")) {
            g_domInvokeSeen.store(true, std::memory_order_release);
            CheckSmokeCompletion();
        }
    }

    void ListenerCallback(const char* argument, void* state) {
        LogState("RegisterJSListenerV2", state);
        logger::info("RegisterJSListenerV2 argument: {}", argument ? argument : "<null>");
        if (ContainsText(argument, "listener-payload")) {
            g_listenerSeen.store(true, std::memory_order_release);
            CheckSmokeCompletion();
        }
    }

    void ConsoleCallback(PrismaView view, PRISMA_UI_API::ConsoleMessageLevel level, const char* message, void* state) {
        LogState("RegisterConsoleCallbackV2", state);
        logger::info("Console callback: view={}, level={}, message={}", view, ConsoleLevelName(level),
                     message ? message : "<null>");
        if (ContainsText(message, "[PrismaUITest]")) {
            g_consoleSeen.store(true, std::memory_order_release);
            CheckSmokeCompletion();
        }
    }

    [[nodiscard]] PrismaView GetEchoView(const std::string_view command, const std::string_view prefix,
                                         std::size_t& index) noexcept {
        if (!command.starts_with(prefix) || command.size() != prefix.size() + 1) {
            return 0;
        }

        const char slot = command[prefix.size()];
        if (slot != '1' && slot != '2') {
            return 0;
        }

        index = static_cast<std::size_t>(slot - '1');
        return g_echoViews[index];
    }

    void ControlCallback(const char* argument, void* state) {
        LogState("View control listener", state);

        if (!g_apiV3) {
            logger::warn("View control skipped: PrismaUI V3 API unavailable");
            return;
        }

        const std::string_view command = argument ? std::string_view(argument) : std::string_view();
        std::size_t index = 0;
        if (const PrismaView view = GetEchoView(command, "focus:"sv, index); view) {
            const bool focused = g_apiV3->Focus(view, false, true);
            logger::info("View control focus echoView{}={} returned {}", index + 1, view, BoolText(focused));
            return;
        }

        if (const PrismaView view = GetEchoView(command, "visibility:"sv, index); view) {
            g_echoViewsVisible[index] = !g_echoViewsVisible[index];
            if (g_echoViewsVisible[index]) {
                g_apiV3->Show(view);
            } else {
                g_apiV3->Hide(view);
            }

            logger::info("View control {} echoView{}={}", g_echoViewsVisible[index] ? "show" : "hide", index + 1, view);
            return;
        }

        logger::warn("View control ignored unknown command '{}'", argument ? argument : "<null>");
    }

    void ReturnFocusCallback(const char* argument, void* state) {
        LogState("Return focus listener", state);

        if (!g_apiV3 || !g_testView) {
            logger::warn("Return focus skipped: api={}, mainView={}", static_cast<void*>(g_apiV3), g_testView);
            return;
        }

        const std::string_view command = argument ? std::string_view(argument) : std::string_view();
        if (command != "focus:main"sv) {
            logger::warn("Return focus ignored unknown command '{}'", argument ? argument : "<null>");
            return;
        }

        const bool focused = g_apiV3->Focus(g_testView, false, true);
        logger::info("Return focus mainView={} returned {}", g_testView, BoolText(focused));
    }

    void RunDomReadyScript(const PrismaView view) {
        if (!g_apiV3 || !view) {
            logger::warn("DOM-ready script skipped: api={}, view={}", static_cast<void*>(g_apiV3), view);
            return;
        }

        g_apiV3->InvokeV2(view,
                          R"JS((() => {
    console.info('[PrismaUITest] DOM-ready InvokeV2 running');
    const listenerType = typeof window.prismaApiTestEcho;
    if (listenerType === 'function') {
        window.prismaApiTestEcho('listener-payload-from-dom-ready-invoke');
    }
    return 'readyState=' + document.readyState + '; listener=' + listenerType + '; interop=' + typeof window.prismaApiTestFunction;
})())JS",
                          DomInvokeCallback, &g_domInvokeState);

        g_apiV3->InteropCall(view, "prismaApiTestFunction", "interop-payload-from-native");
    }

    void DomReadyCallback(PrismaView view, void* state) {
        LogState("CreateViewV2 DOM-ready", state);
        logger::info("CreateViewV2 DOM-ready view={}, expectedCurrentView={}", view, g_testView);
        logger::info("DOM-ready IsValid({})={}", view, BoolText(g_apiV3 && g_apiV3->IsValid(view)));
        g_domReadySeen.store(true, std::memory_order_release);
        CheckSmokeCompletion();
        RunDomReadyScript(view);
        g_apiV3->Focus(view);
    }

    void InitializeLog() {
        auto path = logger::log_directory();
        if (!path) {
            SKSE::stl::report_and_fail("PrismaUITest failed to find the SKSE log directory"sv);
        }

        *path /= "PrismaUITest.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
        auto log = std::make_shared<spdlog::logger>("PrismaUITest", std::move(sink));
        log->set_level(spdlog::level::debug);
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%Y-%m-%d %T.%e] [%l] [%t] [%s:%#] %v");
    }

    void ProbeRawVersions() {
        for (std::uint8_t version = kRawVersionProbeFirst; version <= kRawVersionProbeLast; ++version) {
            const auto* api = PRISMA_UI_API::RequestPluginAPI(static_cast<PRISMA_UI_API::InterfaceVersion>(version));
            logger::info("Raw RequestPluginAPI version {} -> {}", version, api ? "non-null" : "nullptr");
        }
    }

    void AcquireApi() {
        g_apiV1 = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI1>();
        g_apiV2 = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI2>();
        g_apiV3 = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI3>();

        if (!g_apiV3) {
            g_apiV3 = static_cast<PRISMA_UI_API::IVPrismaUI3*>(
                PRISMA_UI_API::RequestPluginAPI(static_cast<PRISMA_UI_API::InterfaceVersion>(6)));
        }

        logger::info("Typed PrismaUI API pointers: V1={}, V2={}, V3={}", static_cast<void*>(g_apiV1),
                     static_cast<void*>(g_apiV2), static_cast<void*>(g_apiV3));
    }

    void RunNullInputTests() {
        if (!g_apiV3) {
            logger::warn("Null-input tests skipped: PrismaUI V3 API unavailable");
            return;
        }

        logger::info("Null-input Focus(0)={}", BoolText(g_apiV3->Focus(0, false, true)));
        logger::info("Null-input HasFocus(0)={}", BoolText(g_apiV3->HasFocus(0)));
        logger::info("Null-input IsValid(0)={}", BoolText(g_apiV3->IsValid(0)));
        logger::info("Null-input IsHidden(0)={}", BoolText(g_apiV3->IsHidden(0)));
        logger::info("Null-input GetOrder(0)={}", g_apiV3->GetOrder(0));
        logger::info("Null-input GetScrollingPixelSize(0)={}", g_apiV3->GetScrollingPixelSize(0));
        g_apiV3->InvokeV2(0, "'invalid-view'", InvokeCallback, &g_invokeState);
        g_apiV3->InteropCall(0, "missing", "invalid-view");
        g_apiV3->RegisterJSListenerV2(0, "missingListener", ListenerCallback, &g_listenerState);
        g_apiV3->RegisterConsoleCallbackV2(0, ConsoleCallback, &g_consoleState);
        g_apiV3->Unfocus(0);
        g_apiV3->Hide(0);
        g_apiV3->Show(0);
        g_apiV3->Destroy(0);
    }

    void RunDisposableViewTest() {
        const PrismaView view = g_apiV3->CreateViewV2(kViewPath, nullptr, nullptr);
        logger::info("Disposable CreateViewV2 returned {}", view);
        logger::info("Disposable IsValid before Destroy={}", BoolText(g_apiV3->IsValid(view)));
        g_apiV3->Destroy(view);
        logger::info("Disposable IsValid after Destroy={}", BoolText(g_apiV3->IsValid(view)));
    }

    void CreateEchoViews() {
        for (std::size_t i = 0; i < 2; ++i) {
            g_echoViews[i] = g_apiV3->CreateViewV2(kEchoViewPaths[i], nullptr, nullptr);
            g_echoViewsVisible[i] = g_echoViews[i] != 0;
            logger::info("CreateViewV2 returned echo view {} for {}", g_echoViews[i], kEchoViewPaths[i]);
            if (!g_echoViews[i]) {
                continue;
            }

            g_apiV3->RegisterConsoleCallbackV2(g_echoViews[i], ConsoleCallback, &g_consoleState);
            g_apiV3->RegisterJSListenerV2(g_echoViews[i], "prismaApiTestReturnFocus", ReturnFocusCallback,
                                          &g_controlState);
            g_apiV3->SetOrder(g_echoViews[i], static_cast<int>(40 + i));
            logger::info("Echo view {} IsValid={}, order={}", i + 1, BoolText(g_apiV3->IsValid(g_echoViews[i])),
                         g_apiV3->GetOrder(g_echoViews[i]));
        }
    }

    void RunViewLifecycleTest() {
        bool expected = false;
        if (!g_startedLifecycleTest.compare_exchange_strong(expected, true)) {
            return;
        }

        if (!g_apiV3) {
            logger::error("View lifecycle test skipped: PrismaUI V3 API unavailable");
            return;
        }

        RunNullInputTests();
        RunDisposableViewTest();
        CreateEchoViews();

        g_testView = g_apiV3->CreateViewV2(kViewPath, DomReadyCallback, &g_domReadyState);
        logger::info("CreateViewV2 returned test view {} for {}", g_testView, kViewPath);
        if (!g_testView) {
            logger::error("CreateViewV2 failed; remaining lifecycle API calls skipped");
            return;
        }

        g_apiV3->RegisterConsoleCallbackV2(g_testView, ConsoleCallback, &g_consoleState);
        g_apiV3->RegisterJSListenerV2(g_testView, "prismaApiTestEcho", ListenerCallback, &g_listenerState);
        g_apiV3->RegisterJSListenerV2(g_testView, "prismaApiTestControl", ControlCallback, &g_controlState);
        logger::info("IsValid({})={}", g_testView, BoolText(g_apiV3->IsValid(g_testView)));
        logger::info("Initial IsHidden({})={}", g_testView, BoolText(g_apiV3->IsHidden(g_testView)));
        logger::info("Initial HasFocus({})={}", g_testView, BoolText(g_apiV3->HasFocus(g_testView)));
        logger::info("Initial HasAnyActiveFocus()={}", BoolText(g_apiV3->HasAnyActiveFocus()));
        logger::info("Initial GetOrder({})={}", g_testView, g_apiV3->GetOrder(g_testView));
        logger::info("Initial GetScrollingPixelSize({})={}", g_testView, g_apiV3->GetScrollingPixelSize(g_testView));

        g_apiV3->SetScrollingPixelSize(g_testView, 48);
        logger::info("After SetScrollingPixelSize(48), GetScrollingPixelSize({})={}", g_testView,
                     g_apiV3->GetScrollingPixelSize(g_testView));

        g_apiV3->SetOrder(g_testView, 42);
        logger::info("After SetOrder(42), GetOrder({})={}", g_testView, g_apiV3->GetOrder(g_testView));

        g_apiV3->Hide(g_testView);
        logger::info("Hide({}) called; IsHidden is operation-queue backed and may update on the next present",
                     g_testView);
        g_apiV3->Show(g_testView);
        logger::info("Show({}) called", g_testView);

        const bool focusResult = g_apiV3->Focus(g_testView, false, true);
        logger::info("Focus({}, pauseGame=false, disableFocusMenu=true) returned {}", g_testView,
                     BoolText(focusResult));
        logger::info("HasFocus({}) immediately after Focus={}", g_testView, BoolText(g_apiV3->HasFocus(g_testView)));
        g_apiV3->Unfocus(g_testView);
        logger::info("Unfocus({}) called", g_testView);

        g_apiV3->CreateInspectorView(g_testView);
        g_apiV3->SetInspectorVisibility(g_testView, true);
        g_apiV3->SetInspectorBounds(g_testView, 16.0F, 16.0F, 640U, 360U);
        logger::info("Legacy inspector IsInspectorVisible({})={}", g_testView,
                     BoolText(g_apiV3->IsInspectorVisible(g_testView)));

        logger::info("DevTools initially open={}", BoolText(g_apiV3->IsDevToolsOpen()));
        g_apiV3->OpenDevTools();
        logger::info("OpenDevTools() called");
        g_apiV3->CloseDevTools();
        logger::info("CloseDevTools() called");

        g_apiV3->InvokeV2(g_testView,
                          R"JS((() => {
    console.log('[PrismaUITest] initial InvokeV2 running');
    return 'initial-readyState=' + document.readyState;
})())JS",
                          InvokeCallback, &g_invokeState);
    }

    void HandleMessage(SKSE::MessagingInterface::Message* message) {
        if (!message) {
            return;
        }

        logger::info("SKSE message received: {} ({})", MessageName(message->type),
                     static_cast<std::uint32_t>(message->type));

        switch (message->type) {
            case SKSE::MessagingInterface::kPostLoad:
                ProbeRawVersions();
                AcquireApi();
                break;
            case SKSE::MessagingInterface::kDataLoaded:
                if (!g_apiV3) {
                    AcquireApi();
                }
                RunViewLifecycleTest();
                break;
            default:
                break;
        }
    }
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse, false);
    InitializeLog();

    logger::info("---------------- PrismaUITest SKSE plugin loaded ----------------");
    logger::info("Built with CommonLibSSE-NG v{}", COMMONLIBSSE_VERSION);
    logger::info("Running on Skyrim v{}", REL::Module::get().version().string());

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging) {
        logger::critical("Failed to query SKSE messaging interface");
        return false;
    }

    if (!messaging->RegisterListener(HandleMessage)) {
        logger::critical("Failed to register SKSE messaging listener");
        return false;
    }

    return true;
}
