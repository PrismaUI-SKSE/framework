#pragma once

#include <Ultralight/Ultralight.h>
#include <Ultralight/View.h>
#include <Ultralight/StringSTL.h>
#include <AppCore/Platform.h>
#include <JavaScriptCore/JSRetainPtr.h>

namespace PrismaUI::Core {
	typedef uint64_t PrismaViewId;
}

namespace PrismaUI::Listeners {
	using namespace ultralight;

	class MyLoadListener : public LoadListener {
		Core::PrismaViewId viewId_;

	public:
		explicit MyLoadListener(Core::PrismaViewId id);
		virtual ~MyLoadListener();

		virtual void OnBeginLoading(View* caller, uint64_t frame_id, bool is_main_frame, const String& url) override;
		virtual void OnFinishLoading(View* caller, uint64_t frame_id, bool is_main_frame, const String& url) override;
		virtual void OnFailLoading(View* caller, uint64_t frame_id, bool is_main_frame, const String& url, const String& description, const String& error_domain, int error_code) override;
		virtual void OnWindowObjectReady(View* caller, uint64_t frame_id, bool is_main_frame, const String& url) override;
		virtual void OnDOMReady(View* caller, uint64_t frame_id, bool is_main_frame, const String& url) override;
	};

	class MyViewListener : public ViewListener {
		Core::PrismaViewId viewId_;

	public:
		explicit MyViewListener(Core::PrismaViewId id);
		virtual ~MyViewListener();

		virtual void OnAddConsoleMessage(View* caller, const ConsoleMessage& message) override;
		virtual RefPtr<View> OnCreateInspectorView(View* caller, bool is_local, const String& inspectedURL) override;
	};

	class MyUltralightLogger : public Logger {
	public:
		virtual ~MyUltralightLogger();
		virtual void LogMessage(LogLevel log_level, const String& message) override;
	};
}
