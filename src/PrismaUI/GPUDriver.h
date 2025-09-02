#pragma once

#include <Ultralight/Ultralight.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <map>
#include <vector>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace PrismaUI {

class GPUDriver : public ultralight::GPUDriver {
public:
    GPUDriver(ID3D11Device* device, ID3D11DeviceContext* context);
    virtual ~GPUDriver();

    void BeginSynchronize() override;
    void EndSynchronize() override;

    uint32_t NextTextureId() override;
    void CreateTexture(uint32_t texture_id, ultralight::RefPtr<ultralight::Bitmap> bitmap) override;
    void UpdateTexture(uint32_t texture_id, ultralight::RefPtr<ultralight::Bitmap> bitmap) override;
    void DestroyTexture(uint32_t texture_id) override;

    uint32_t NextRenderBufferId() override;
    void CreateRenderBuffer(uint32_t render_buffer_id, const ultralight::RenderBuffer& buffer) override;
    void DestroyRenderBuffer(uint32_t render_buffer_id) override;

    uint32_t NextGeometryId() override;
    void CreateGeometry(uint32_t geometry_id, const ultralight::VertexBuffer& vertices, const ultralight::IndexBuffer& indices) override;
    void UpdateGeometry(uint32_t geometry_id, const ultralight::VertexBuffer& vertices, const ultralight::IndexBuffer& indices) override;
    void DestroyGeometry(uint32_t geometry_id) override;

    void UpdateCommandList(const ultralight::CommandList& list) override;

    bool HasCommands() const { return !command_list_.empty(); }
    void DrawCommandList();

    ID3D11ShaderResourceView* GetShaderResourceView(uint32_t texture_id);

    void ClearRenderBuffer(uint32_t render_buffer_id);

protected:
    void DrawGeometry(uint32_t geometry_id, uint32_t indices_count, uint32_t indices_offset, const ultralight::GPUState& state);
    void BindTexture(uint8_t texture_unit, uint32_t texture_id);

    void LoadShaders();
    void BindShader(ultralight::ShaderType shader);
    void BindVertexLayout(ultralight::VertexBufferFormat format);
    void BindGeometry(uint32_t id);
    ID3D11RenderTargetView* GetRenderTargetView(uint32_t render_buffer_id);
    ComPtr<ID3D11SamplerState> GetSamplerState();
    ComPtr<ID3D11Buffer> GetConstantBuffer();
    void SetViewport(uint32_t width, uint32_t height);
    void UpdateConstantBuffer(const ultralight::GPUState& state);
    ultralight::Matrix ApplyProjection(const ultralight::Matrix4x4& transform, float screen_width, float screen_height);

    ID3D11Device* device_;
    ID3D11DeviceContext* context_;

    ComPtr<ID3D11InputLayout> vertex_layout_2f_4ub_2f_;
    ComPtr<ID3D11InputLayout> vertex_layout_2f_4ub_2f_2f_28f_;
    ComPtr<ID3D11SamplerState> sampler_state_;
    ComPtr<ID3D11Buffer> constant_buffer_;
    ComPtr<ID3D11BlendState> blend_state_normal_;
    ComPtr<ID3D11BlendState> blend_state_premultiplied_;
    ComPtr<ID3D11RasterizerState> rasterizer_state_cull_none_scissor_;
    ComPtr<ID3D11RasterizerState> rasterizer_state_cull_none_no_scissor_;

    struct GeometryEntry {
        ultralight::VertexBufferFormat format;
        ComPtr<ID3D11Buffer> vertexBuffer;
        ComPtr<ID3D11Buffer> indexBuffer;
    };
    std::map<uint32_t, GeometryEntry> geometry_;

    struct TextureEntry {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> texture_srv;
    };
    std::map<uint32_t, TextureEntry> textures_;

    struct RenderTargetEntry {
        ComPtr<ID3D11RenderTargetView> render_target_view;
    };
    std::map<uint32_t, RenderTargetEntry> render_targets_;

    std::map<ultralight::ShaderType, std::pair<ComPtr<ID3D11VertexShader>, ComPtr<ID3D11PixelShader>>> shaders_;

    std::vector<ultralight::Command> command_list_;
    std::mutex command_list_mutex_;

    uint32_t next_texture_id_ = 1;
    uint32_t next_render_buffer_id_ = 1;
    uint32_t next_geometry_id_ = 1;
};

} // namespace PrismaUI
