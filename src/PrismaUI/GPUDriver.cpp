#include "PCH.h"
#include "GPUDriver.h"
#include "Core.h"
#include <DirectXColors.h>
#include <d3dcompiler.h>
#include <vector>

// Include pre-compiled shaders
#include "../../lib/ultralight-1.4.0/shaders/hlsl/bin/fill_fxc.h"
#include "../../lib/ultralight-1.4.0/shaders/hlsl/bin/fill_path_fxc.h"
#include "../../lib/ultralight-1.4.0/shaders/hlsl/bin/v2f_c4f_t2f_fxc.h"
#include "../../lib/ultralight-1.4.0/shaders/hlsl/bin/v2f_c4f_t2f_t2f_d28f_fxc.h"

namespace {

struct Vertex_2f_4ub_2f {
    float pos[2];
    uint8_t color[4];
    float obj[2];
};

struct Vertex_2f_4ub_2f_2f_28f {
    float pos[2];
    uint8_t color[4];
    float tex[2];
    float obj[2];
    float data[28];
};

struct Uniforms {
    ultralight::Matrix4x4 Transform;
    ultralight::vec4 State;
    ultralight::vec4 Scalar4[2];
    ultralight::vec4 Vector[8];
    uint32_t ClipSize;
    float pad[3];
    ultralight::Matrix4x4 Clip[8];
};

} // namespace

namespace PrismaUI {

GPUDriver::GPUDriver(ID3D11Device* device, ID3D11DeviceContext* context)
    : device_(device), context_(context) {
    LoadShaders();

    D3D11_RASTERIZER_DESC rasterizer_desc = {};
    rasterizer_desc.FillMode = D3D11_FILL_SOLID;
    rasterizer_desc.CullMode = D3D11_CULL_NONE;
    rasterizer_desc.FrontCounterClockwise = false;
    rasterizer_desc.DepthClipEnable = true;
    rasterizer_desc.ScissorEnable = true;
    device_->CreateRasterizerState(&rasterizer_desc, &rasterizer_state_cull_none_scissor_);

    rasterizer_desc.ScissorEnable = false;
    device_->CreateRasterizerState(&rasterizer_desc, &rasterizer_state_cull_none_no_scissor_);

    D3D11_BLEND_DESC blend_desc = {};
    blend_desc.RenderTarget[0].BlendEnable = true;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&blend_desc, &blend_state_normal_);

    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    device_->CreateBlendState(&blend_desc, &blend_state_premultiplied_);
}

GPUDriver::~GPUDriver() {}

void GPUDriver::BeginSynchronize() {
    // logger::debug("GPUDriver::BeginSynchronize");
}
void GPUDriver::EndSynchronize() {
    // logger::debug("GPUDriver::EndSynchronize");
}

uint32_t GPUDriver::NextTextureId() { 
    uint32_t id = next_texture_id_++;
    logger::debug("GPUDriver::NextTextureId -> {}", id);
    return id;
}

void GPUDriver::CreateTexture(uint32_t texture_id, ultralight::RefPtr<ultralight::Bitmap> bitmap) {
    logger::debug("GPUDriver::CreateTexture, id: {}, width: {}, height: {}, is_empty: {}", texture_id, bitmap->width(), bitmap->height(), bitmap->IsEmpty());
    if (textures_.count(texture_id)) {
        logger::error("GPUDriver::CreateTexture, texture id already exists.");
        return;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = bitmap->width();
    desc.Height = bitmap->height();
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = bitmap->format() == ultralight::BitmapFormat::A8_UNORM ? DXGI_FORMAT_A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;

    auto& texture_entry = textures_[texture_id];
    HRESULT hr;

    if (bitmap->IsEmpty()) {
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        hr = device_->CreateTexture2D(&desc, nullptr, &texture_entry.texture);
    } else {
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SUBRESOURCE_DATA tex_data = {};
        tex_data.pSysMem = bitmap->LockPixels();
        tex_data.SysMemPitch = bitmap->row_bytes();
        hr = device_->CreateTexture2D(&desc, &tex_data, &texture_entry.texture);
        bitmap->UnlockPixels();
    }

    if (FAILED(hr)) {
        logger::error("GPUDriver::CreateTexture, unable to create texture. HR={:#X}", hr);
        textures_.erase(texture_id);
        return;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = 1;
    hr = device_->CreateShaderResourceView(texture_entry.texture.Get(), &srv_desc, &texture_entry.texture_srv);
    if (FAILED(hr)) {
        logger::error("GPUDriver::CreateTexture, unable to create shader resource view for texture. HR={:#X}", hr);
        textures_.erase(texture_id);
    }
}

void GPUDriver::UpdateTexture(uint32_t texture_id, ultralight::RefPtr<ultralight::Bitmap> bitmap) {
    logger::debug("GPUDriver::UpdateTexture, id: {}", texture_id);
    auto it = textures_.find(texture_id);
    if (it == textures_.end()) {
        logger::error("GPUDriver::UpdateTexture, texture id doesn't exist.");
        return;
    }

    D3D11_MAPPED_SUBRESOURCE res;
    context_->Map(it->second.texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res);
    memcpy(res.pData, bitmap->LockPixels(), bitmap->size());
    bitmap->UnlockPixels();
    context_->Unmap(it->second.texture.Get(), 0);
}

void GPUDriver::BindTexture(uint8_t texture_unit, uint32_t texture_id) {
    auto it = textures_.find(texture_id);
    if (it == textures_.end()) return;
    context_->PSSetShaderResources(texture_unit, 1, it->second.texture_srv.GetAddressOf());
}

void GPUDriver::DestroyTexture(uint32_t texture_id) {
    logger::debug("GPUDriver::DestroyTexture, id: {}", texture_id);
    textures_.erase(texture_id);
}

uint32_t GPUDriver::NextRenderBufferId() { 
    uint32_t id = next_render_buffer_id_++;
    logger::debug("GPUDriver::NextRenderBufferId -> {}", id);
    return id;
}

void GPUDriver::CreateRenderBuffer(uint32_t render_buffer_id, const ultralight::RenderBuffer& buffer) {
    logger::debug("GPUDriver::CreateRenderBuffer, id: {}, texture_id: {}", render_buffer_id, buffer.texture_id);
    auto it = textures_.find(buffer.texture_id);
    if (it == textures_.end()) {
        logger::error("GPUDriver::CreateRenderBuffer, texture id {} doesn't exist.", buffer.texture_id);
        return;
    }

    auto& texture_entry = it->second;
    
    D3D11_TEXTURE2D_DESC desc;
    texture_entry.texture->GetDesc(&desc);

    auto& render_target_entry = render_targets_[render_buffer_id];
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = desc.Format;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    HRESULT hr = device_->CreateRenderTargetView(texture_entry.texture.Get(), &rtv_desc, &render_target_entry.render_target_view);
    if (FAILED(hr)) {
        logger::error("GPUDriver::CreateRenderBuffer, unable to create render target view. HR={:#X}", hr);
    }
}

void GPUDriver::ClearRenderBuffer(uint32_t render_buffer_id) {
    ID3D11RenderTargetView* target = GetRenderTargetView(render_buffer_id);
    if (!target) {
        logger::error("GPUDriver::ClearRenderBuffer, render buffer id {} doesn't exist.", render_buffer_id);
        return;
    }
    context_->ClearRenderTargetView(target, DirectX::Colors::Transparent);
}

void GPUDriver::DestroyRenderBuffer(uint32_t render_buffer_id) {
    logger::debug("GPUDriver::DestroyRenderBuffer, id: {}", render_buffer_id);
    render_targets_.erase(render_buffer_id);
}

uint32_t GPUDriver::NextGeometryId() { 
    uint32_t id = next_geometry_id_++;
    logger::debug("GPUDriver::NextGeometryId -> {}", id);
    return id;
}

void GPUDriver::CreateGeometry(uint32_t geometry_id, const ultralight::VertexBuffer& vertices, const ultralight::IndexBuffer& indices) {
    logger::debug("GPUDriver::CreateGeometry, id: {}", geometry_id);
    GeometryEntry geometry;
    geometry.format = vertices.format;

    D3D11_BUFFER_DESC desc = {};
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.ByteWidth = vertices.size;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA data = { vertices.data, 0, 0 };
    device_->CreateBuffer(&desc, &data, &geometry.vertexBuffer);

    desc.ByteWidth = indices.size;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    data.pSysMem = indices.data;
    device_->CreateBuffer(&desc, &data, &geometry.indexBuffer);

    geometry_[geometry_id] = std::move(geometry);
}

void GPUDriver::UpdateGeometry(uint32_t geometry_id, const ultralight::VertexBuffer& vertices, const ultralight::IndexBuffer& indices) {
    logger::debug("GPUDriver::UpdateGeometry, id: {}", geometry_id);
    auto it = geometry_.find(geometry_id);
    if (it == geometry_.end()) return;

    D3D11_MAPPED_SUBRESOURCE res;
    context_->Map(it->second.vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res);
    memcpy(res.pData, vertices.data, vertices.size);
    context_->Unmap(it->second.vertexBuffer.Get(), 0);

    context_->Map(it->second.indexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res);
    memcpy(res.pData, indices.data, indices.size);
    context_->Unmap(it->second.indexBuffer.Get(), 0);
}

void GPUDriver::DrawGeometry(uint32_t geometry_id, uint32_t indices_count, uint32_t indices_offset, const ultralight::GPUState& state) {
    ID3D11RenderTargetView* target = GetRenderTargetView(state.render_buffer_id);
    if (!target) {
        logger::error("GPUDriver::DrawGeometry, render buffer id {} doesn't exist.", state.render_buffer_id);
        return;
    }
    context_->OMSetRenderTargets(1, &target, nullptr);

    SetViewport(state.viewport_width, state.viewport_height);
    UpdateConstantBuffer(state);
    BindGeometry(geometry_id);
    BindShader(state.shader_type);

    if (state.texture_1_id) BindTexture(0, state.texture_1_id);
    if (state.texture_2_id) BindTexture(1, state.texture_2_id);
    if (state.texture_3_id) BindTexture(2, state.texture_3_id);

    auto sampler_state = GetSamplerState();
    context_->PSSetSamplers(0, 1, sampler_state.GetAddressOf());

    if (state.enable_blend) {
        context_->OMSetBlendState(blend_state_normal_.Get(), nullptr, 0xffffffff);
    } else {
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    }

    if (state.enable_scissor) {
        context_->RSSetState(rasterizer_state_cull_none_scissor_.Get());
        D3D11_RECT scissor_rect = { (LONG)state.scissor_rect.left, (LONG)state.scissor_rect.top, (LONG)state.scissor_rect.right, (LONG)state.scissor_rect.bottom };
        context_->RSSetScissorRects(1, &scissor_rect);
    } else {
        context_->RSSetState(rasterizer_state_cull_none_no_scissor_.Get());
    }

    context_->VSSetConstantBuffers(0, 1, constant_buffer_.GetAddressOf());
    context_->PSSetConstantBuffers(0, 1, constant_buffer_.GetAddressOf());
    context_->DrawIndexed(indices_count, indices_offset, 0);
}

void GPUDriver::DestroyGeometry(uint32_t geometry_id) {
    logger::debug("GPUDriver::DestroyGeometry, id: {}", geometry_id);
    geometry_.erase(geometry_id);
}

void GPUDriver::UpdateCommandList(const ultralight::CommandList& list) {
    // logger::debug("GPUDriver::UpdateCommandList, num_commands: {}", list.size);
    std::lock_guard<std::mutex> lock(command_list_mutex_);
    command_list_.assign(list.commands, list.commands + list.size);
}

void GPUDriver::DrawCommandList() {
    std::vector<ultralight::Command> list;
    {
        std::lock_guard<std::mutex> lock(command_list_mutex_);
        if (command_list_.empty()) return;
        list = std::move(command_list_);
    }

    // Save original state
    ID3D11RenderTargetView*   backup_rtv = nullptr;
    ID3D11DepthStencilView*   backup_dsv = nullptr;
    ID3D11BlendState*         backup_blend_state = nullptr;
    FLOAT                     backup_blend_factor[4];
    UINT                      backup_sample_mask = 0;
    ID3D11DepthStencilState*  backup_depth_stencil_state = nullptr;
    UINT                      backup_stencil_ref = 0;
    ID3D11RasterizerState*    backup_rasterizer_state = nullptr;
    ID3D11InputLayout*        backup_input_layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY  backup_primitive_topology;
    ID3D11Buffer*             backup_vb = nullptr;
    UINT                      backup_vb_stride = 0;
    UINT                      backup_vb_offset = 0;
    ID3D11Buffer*             backup_ib = nullptr;
    DXGI_FORMAT               backup_ib_format = DXGI_FORMAT_UNKNOWN;
    UINT                      backup_ib_offset = 0;
    ID3D11VertexShader*       backup_vs = nullptr;
    ID3D11PixelShader*        backup_ps = nullptr;
    ID3D11SamplerState*       backup_sampler_state = nullptr;
    ID3D11Buffer*             backup_constant_buffer_vs = nullptr;
    ID3D11Buffer*             backup_constant_buffer_ps = nullptr;
    UINT                      num_viewports = 1;
    D3D11_VIEWPORT            backup_viewport;

    context_->OMGetRenderTargets(1, &backup_rtv, &backup_dsv);
    context_->OMGetBlendState(&backup_blend_state, backup_blend_factor, &backup_sample_mask);
    context_->OMGetDepthStencilState(&backup_depth_stencil_state, &backup_stencil_ref);
    context_->RSGetState(&backup_rasterizer_state);
    context_->IAGetInputLayout(&backup_input_layout);
    context_->IAGetPrimitiveTopology(&backup_primitive_topology);
    context_->IAGetVertexBuffers(0, 1, &backup_vb, &backup_vb_stride, &backup_vb_offset);
    context_->IAGetIndexBuffer(&backup_ib, &backup_ib_format, &backup_ib_offset);
    context_->VSGetShader(&backup_vs, nullptr, nullptr);
    context_->PSGetShader(&backup_ps, nullptr, nullptr);
    context_->PSGetSamplers(0, 1, &backup_sampler_state);
    context_->VSGetConstantBuffers(0, 1, &backup_constant_buffer_vs);
    context_->PSGetConstantBuffers(0, 1, &backup_constant_buffer_ps);
    context_->RSGetViewports(&num_viewports, &backup_viewport);

    // logger::debug("GPUDriver::DrawCommandList, num_commands: {}", list.size());
    for (const auto& command : list) {
        if (command.command_type == ultralight::CommandType::DrawGeometry) {
            DrawGeometry(command.geometry_id, command.indices_count, command.indices_offset, command.gpu_state);
        } else if (command.command_type == ultralight::CommandType::ClearRenderBuffer) {
            ClearRenderBuffer(command.gpu_state.render_buffer_id);
        }
    }

    // Restore original state
    context_->OMSetRenderTargets(1, &backup_rtv, backup_dsv);
    context_->OMSetBlendState(backup_blend_state, backup_blend_factor, backup_sample_mask);
    context_->OMSetDepthStencilState(backup_depth_stencil_state, backup_stencil_ref);
    context_->RSSetState(backup_rasterizer_state);
    context_->IASetInputLayout(backup_input_layout);
    context_->IASetPrimitiveTopology(backup_primitive_topology);
    context_->IASetVertexBuffers(0, 1, &backup_vb, &backup_vb_stride, &backup_vb_offset);
    context_->IASetIndexBuffer(backup_ib, backup_ib_format, backup_ib_offset);
    context_->VSSetShader(backup_vs, nullptr, 0);
    context_->PSSetShader(backup_ps, nullptr, 0);
    context_->PSSetSamplers(0, 1, &backup_sampler_state);
    context_->VSSetConstantBuffers(0, 1, &backup_constant_buffer_vs);
    context_->PSSetConstantBuffers(0, 1, &backup_constant_buffer_ps);
    context_->RSSetViewports(1, &backup_viewport);

    // Release acquired references
    if (backup_rtv) backup_rtv->Release();
    if (backup_dsv) backup_dsv->Release();
    if (backup_blend_state) backup_blend_state->Release();
    if (backup_depth_stencil_state) backup_depth_stencil_state->Release();
    if (backup_rasterizer_state) backup_rasterizer_state->Release();
    if (backup_input_layout) backup_input_layout->Release();
    if (backup_vb) backup_vb->Release();
    if (backup_ib) backup_ib->Release();
    if (backup_vs) backup_vs->Release();
    if (backup_ps) backup_ps->Release();
    if (backup_sampler_state) backup_sampler_state->Release();
    if (backup_constant_buffer_vs) backup_constant_buffer_vs->Release();
    if (backup_constant_buffer_ps) backup_constant_buffer_ps->Release();
}

ID3D11ShaderResourceView* GPUDriver::GetShaderResourceView(uint32_t texture_id) {
    auto it = textures_.find(texture_id);
    if (it != textures_.end()) {
        // logger::debug("GPUDriver::GetShaderResourceView, found SRV for texture_id: {}", texture_id);
        return it->second.texture_srv.Get();
    }
    // logger::warn("GPUDriver::GetShaderResourceView, SRV for texture_id: {} NOT FOUND", texture_id);
    return nullptr;
}

void GPUDriver::LoadShaders() {
    if (!shaders_.empty()) return;

    const D3D11_INPUT_ELEMENT_DESC layout_2f_4ub_2f[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    auto& shader_fill_path = shaders_[ultralight::ShaderType::FillPath];
    device_->CreateVertexShader(v2f_c4f_t2f_fxc, sizeof(v2f_c4f_t2f_fxc), nullptr, &shader_fill_path.first);
    device_->CreateInputLayout(layout_2f_4ub_2f, ARRAYSIZE(layout_2f_4ub_2f), v2f_c4f_t2f_fxc, sizeof(v2f_c4f_t2f_fxc), &vertex_layout_2f_4ub_2f_);
    device_->CreatePixelShader(fill_path_fxc, sizeof(fill_path_fxc), nullptr, &shader_fill_path.second);

    const D3D11_INPUT_ELEMENT_DESC layout_2f_4ub_2f_2f_28f[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 6, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 7, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 8, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    auto& shader_fill = shaders_[ultralight::ShaderType::Fill];
    device_->CreateVertexShader(v2f_c4f_t2f_t2f_d28f_fxc, sizeof(v2f_c4f_t2f_t2f_d28f_fxc), nullptr, &shader_fill.first);
    device_->CreateInputLayout(layout_2f_4ub_2f_2f_28f, ARRAYSIZE(layout_2f_4ub_2f_2f_28f), v2f_c4f_t2f_t2f_d28f_fxc, sizeof(v2f_c4f_t2f_t2f_d28f_fxc), &vertex_layout_2f_4ub_2f_2f_28f_);
    device_->CreatePixelShader(fill_fxc, sizeof(fill_fxc), nullptr, &shader_fill.second);
}

void GPUDriver::BindShader(ultralight::ShaderType shader) {
    auto it = shaders_.find(shader);
    if (it == shaders_.end()) return;
    context_->VSSetShader(it->second.first.Get(), nullptr, 0);
    context_->PSSetShader(it->second.second.Get(), nullptr, 0);
}

void GPUDriver::BindVertexLayout(ultralight::VertexBufferFormat format) {
    if (format == ultralight::VertexBufferFormat::_2f_4ub_2f) {
        context_->IASetInputLayout(vertex_layout_2f_4ub_2f_.Get());
    } else {
        context_->IASetInputLayout(vertex_layout_2f_4ub_2f_2f_28f_.Get());
    }
}

void GPUDriver::BindGeometry(uint32_t id) {
    auto it = geometry_.find(id);
    if (it == geometry_.end()) return;

    auto& geometry = it->second;
    UINT stride = geometry.format == ultralight::VertexBufferFormat::_2f_4ub_2f ? sizeof(Vertex_2f_4ub_2f) : sizeof(Vertex_2f_4ub_2f_2f_28f);
    UINT offset = 0;
    context_->IASetVertexBuffers(0, 1, geometry.vertexBuffer.GetAddressOf(), &stride, &offset);
    context_->IASetIndexBuffer(geometry.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    BindVertexLayout(geometry.format);
}

ID3D11RenderTargetView* GPUDriver::GetRenderTargetView(uint32_t render_buffer_id) {
    auto it = render_targets_.find(render_buffer_id);
    if (it != render_targets_.end()) {
        return it->second.render_target_view.Get();
    }
    return nullptr;
}

ComPtr<ID3D11SamplerState> GPUDriver::GetSamplerState() {
    if (sampler_state_) return sampler_state_;

    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MinLOD = 0;
    desc.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&desc, &sampler_state_);
    return sampler_state_;
}

ComPtr<ID3D11Buffer> GPUDriver::GetConstantBuffer() {
    if (constant_buffer_) return constant_buffer_;

    D3D11_BUFFER_DESC desc = {};
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.ByteWidth = sizeof(Uniforms);
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&desc, nullptr, &constant_buffer_);
    return constant_buffer_;
}

void GPUDriver::SetViewport(uint32_t width, uint32_t height) {
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);
}

void GPUDriver::UpdateConstantBuffer(const ultralight::GPUState& state) {
    auto buffer = GetConstantBuffer();
    Uniforms uniforms;
    ultralight::Matrix m = ApplyProjection(state.transform, (float)state.viewport_width, (float)state.viewport_height);
    memcpy(&uniforms.Transform, &m, sizeof(m));
    uniforms.State = { (float)state.viewport_width, (float)state.viewport_height, 1.0f, 1.0f };
    memcpy(uniforms.Scalar4, state.uniform_scalar, sizeof(uniforms.Scalar4));
    memcpy(uniforms.Vector, state.uniform_vector, sizeof(uniforms.Vector));
    uniforms.ClipSize = state.clip_size;
    memcpy(uniforms.Clip, state.clip, state.clip_size * sizeof(ultralight::Matrix4x4));

    D3D11_MAPPED_SUBRESOURCE res;
    context_->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &res);
    memcpy(res.pData, &uniforms, sizeof(Uniforms));
    context_->Unmap(buffer.Get(), 0);
}

ultralight::Matrix GPUDriver::ApplyProjection(const ultralight::Matrix4x4& transform, float screen_width, float screen_height) {
    ultralight::Matrix result;
    result.SetOrthographicProjection(screen_width, screen_height, false);
    ultralight::Matrix transform_mat;
    transform_mat.Set(transform);
    result.Transform(transform_mat);
    return result;
}

} // namespace PrismaUI
