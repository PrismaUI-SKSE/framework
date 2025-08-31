#include "Listeners.h"
#include "Core.h"
#include "Communication.h"

namespace PrismaUI::Listeners {
	using namespace Core;
	using namespace Communication;

	// MyLoadListener
	MyLoadListener::MyLoadListener(Core::PrismaViewId id) : viewId_(std::move(id)) {}

	MyLoadListener::~MyLoadListener() = default;

	void MyLoadListener::OnBeginLoading(View* caller, uint64_t frame_id, bool is_main_frame, const String& url) {
		logger::info("View [{}]: LoadListener: Begin loading URL: {}", viewId_, url.utf8().data());
		uiThread.submit([id = viewId_] {
			std::shared_lock lock(viewsMutex);
			auto it = views.find(id);
			if (it != views.end()) {
				it->second->isLoadingFinished = false;
			}
		});
	}

	void MyLoadListener::OnFinishLoading(View* caller, uint64_t frame_id, bool is_main_frame, const String& url) {
		logger::info("View [{}]: LoadListener: Finished loading URL: {}", viewId_, url.utf8().data());
		uiThread.submit([id = viewId_] {
			std::shared_lock lock(viewsMutex);
			auto it = views.find(id);
			if (it != views.end()) {
				it->second->isLoadingFinished = true;
				Communication::BindJSCallbacks(id);
			}
		});
	}

	void MyLoadListener::OnFailLoading(View* caller, uint64_t frame_id, bool is_main_frame, const String& url, const String& description, const String& error_domain, int error_code) {
		logger::error("View [{}]: LoadListener: Failed loading URL: {}. Error: {}", viewId_, url.utf8().data(), description.utf8().data());
		uiThread.submit([id = viewId_] {
			std::shared_lock lock(viewsMutex);
			auto it = views.find(id);
			if (it != views.end()) {
				it->second->isLoadingFinished = false;
			}
		});
	}

	void MyLoadListener::OnWindowObjectReady(View* caller, uint64_t frame_id, bool is_main_frame, const String& url) {
		logger::info("View [{}]: LoadListener: Window object ready.", viewId_);
	}

	void MyLoadListener::OnDOMReady(View* caller, uint64_t frame_id, bool is_main_frame, const String& url) {
		logger::info("View [{}]: LoadListener: DOM ready.", viewId_);
		uiThread.submit([id = viewId_] {
			std::shared_lock lock(viewsMutex);
			auto it = views.find(id);
			if (it != views.end() && it->second->domReadyCallback) {
				it->second->domReadyCallback(id);
			}
		});
	}

	// MyViewListener
	MyViewListener::MyViewListener(Core::PrismaViewId id) : viewId_(std::move(id)) {}

	MyViewListener::~MyViewListener() = default;

	void MyViewListener::OnAddConsoleMessage(View* caller, const ConsoleMessage& message) {
		logger::info("View [{}]: JSConsole: {}", viewId_, message.message().utf8().data());
	}

	// MyUltralightLogger
	MyUltralightLogger::~MyUltralightLogger() = default;

	void MyUltralightLogger::LogMessage(LogLevel log_level, const String& message) {
		// Implementation was empty, so keep it empty.
	}
}
