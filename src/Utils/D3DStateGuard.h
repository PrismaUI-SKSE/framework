#pragma once

#include <d3d11.h>

namespace PrismaUI::Utils
{
    // RAII guard that snapshots and restores the OM blend state, OM depth-stencil
    // state, and RS rasterizer state of a D3D11 immediate context. Wrap any
    // SpriteBatch (or otherwise pipeline-mutating) pass with one of these so the
    // surrounding game state is left exactly as it was found.
    //
    // The guard takes ownership of the COM references returned by OMGetBlendState /
    // OMGetDepthStencilState / RSGetState and releases them in its destructor.
    class D3DStateGuard
    {
    public:
        explicit D3DStateGuard(ID3D11DeviceContext* context) : context_(context)
        {
            if (!context_) return;
            context_->OMGetBlendState(&blendState_, blendFactor_, &sampleMask_);
            context_->OMGetDepthStencilState(&depthStencilState_, &stencilRef_);
            context_->RSGetState(&rasterizerState_);
        }

        ~D3DStateGuard()
        {
            if (!context_) return;
            context_->OMSetBlendState(blendState_, blendFactor_, sampleMask_);
            context_->OMSetDepthStencilState(depthStencilState_, stencilRef_);
            context_->RSSetState(rasterizerState_);

            if (blendState_) blendState_->Release();
            if (depthStencilState_) depthStencilState_->Release();
            if (rasterizerState_) rasterizerState_->Release();
        }

        D3DStateGuard(const D3DStateGuard&) = delete;
        D3DStateGuard& operator=(const D3DStateGuard&) = delete;

    private:
        ID3D11DeviceContext* context_ = nullptr;
        ID3D11BlendState* blendState_ = nullptr;
        FLOAT blendFactor_[4] = {};
        UINT sampleMask_ = 0;
        ID3D11DepthStencilState* depthStencilState_ = nullptr;
        UINT stencilRef_ = 0;
        ID3D11RasterizerState* rasterizerState_ = nullptr;
    };
}
