#pragma once

#include <d3d11.h>
#include <wrl/client.h>

namespace test_d3d11 {

// An occlusion query with no draws sees no samples. Leave the game's
// predicate bound across the layer call, including all copies and resolves.
inline Microsoft::WRL::ComPtr<ID3D11Predicate> BindOcclusionPredicate(ID3D11Device* device,
                                                                      ID3D11DeviceContext* context,
                                                                      BOOL value) {
    Microsoft::WRL::ComPtr<ID3D11Predicate> predicate;
    D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
    if (SUCCEEDED(device->CreatePredicate(&desc, &predicate))) {
        context->Begin(predicate.Get());
        context->End(predicate.Get());
        context->SetPredication(predicate.Get(), value);
    }
    return predicate;
}

inline bool CheckAndClearPredicate(ID3D11DeviceContext* context, ID3D11Predicate* expected, BOOL expected_value) {
    Microsoft::WRL::ComPtr<ID3D11Predicate> actual;
    BOOL value = FALSE;
    context->GetPredication(&actual, &value);
    context->SetPredication(nullptr, FALSE);
    return expected && actual.Get() == expected && value == expected_value;
}

} // namespace test_d3d11
