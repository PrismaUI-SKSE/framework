#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

namespace PrismaUI::Utils {
    inline HRESULT CopyResourceAndWait(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Resource* destination,
                                       ID3D11Resource* source) {
        if (!device || !context || !destination || !source) {
            return E_INVALIDARG;
        }

        D3D11_QUERY_DESC queryDesc{};
        queryDesc.Query = D3D11_QUERY_EVENT;

        Microsoft::WRL::ComPtr<ID3D11Query> query;
        HRESULT hr = device->CreateQuery(&queryDesc, query.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            return hr;
        }

        context->CopyResource(destination, source);
        context->End(query.Get());

        BOOL complete = FALSE;
        while ((hr = context->GetData(query.Get(), &complete, sizeof(complete), 0)) == S_FALSE) {
            SwitchToThread();
        }

        return hr;
    }
}
