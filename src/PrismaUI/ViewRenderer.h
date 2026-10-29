#pragma once

#pragma warning(push)
#pragma warning(disable : 4100)
#include <AppCore/Platform.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <Ultralight/StringSTL.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/View.h>
#pragma warning(pop)

#include <d3d11.h>

namespace PrismaUI::Core {
    struct PrismaView;
}

namespace PrismaUI::ViewRenderer {
    using namespace ultralight;

    void UpdateLogic();
    void RenderViews();
    void RenderSingleView(std::shared_ptr<Core::PrismaView> viewData);
    void CopyBitmapToBuffer(std::shared_ptr<Core::PrismaView> viewData, const IntRect& dirtyBounds);
    void DrawViews();
    void UpdateSingleTextureFromBuffer(std::shared_ptr<Core::PrismaView> viewData);
    void CopyPixelsToTexture(Core::PrismaView* viewData, void* pixels, uint32_t width, uint32_t height,
                             uint32_t stride, D3D11_BOX dirty);
    void DrawSingleTexture(std::shared_ptr<Core::PrismaView> viewData);
    void DrawCursor();
    void ReleaseViewTexture(Core::PrismaView* viewData);
}
