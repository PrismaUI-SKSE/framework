#pragma once

#pragma warning(push)
#pragma warning(disable : 4100)
#include <AppCore/Platform.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <Ultralight/StringSTL.h>
#include <Ultralight/Ultralight.h>
#include <Ultralight/View.h>
#pragma warning(pop)

namespace PrismaUI::Core {
    struct PrismaView;
}

namespace PrismaUI::ViewRenderer {
    using namespace ultralight;

    void UpdateLogic();
    void RenderViews();
    void RenderSingleView(std::shared_ptr<Core::PrismaView> viewData);
    void CopyBitmapToBuffer(std::shared_ptr<Core::PrismaView> viewData);
    /// Draws views still presented by PrismaUI itself.
    void DrawViews();
    /// Copies hosted views and their model previews into external surfaces.
    void ComposeExternalSurfaces();
    void UpdateSingleTextureFromBuffer(std::shared_ptr<Core::PrismaView> viewData);
    /// Uploads a BGRA frame and recreates synchronized textures when dimensions change.
    void CopyPixelsToTexture(Core::PrismaView* viewData, void* pixels, uint32_t width, uint32_t height,
                             uint32_t stride);
    void DrawSingleTexture(std::shared_ptr<Core::PrismaView> viewData);
    void DrawCursor();
    /// Releases all D3D11 resources owned by a view.
    void ReleaseViewTexture(Core::PrismaView* viewData);
    /// Releases only the composite surface owned by an external host.
    void ReleaseExternalSurface(Core::PrismaView* viewData);
}
