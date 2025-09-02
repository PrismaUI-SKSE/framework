#pragma once

#include <Ultralight/Ultralight.h>
#include <Ultralight/View.h>
#include <Ultralight/StringSTL.h>
#include <AppCore/Platform.h>
#include <JavaScriptCore/JSRetainPtr.h>

namespace PrismaUI::Core {
	struct PrismaView;
}

namespace PrismaUI::ViewRenderer {
	using namespace ultralight;

	void DrawViews();
	void DrawCursor();
}
