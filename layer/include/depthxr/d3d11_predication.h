#pragma once

#include <d3d11.h>
#include <wrl/client.h>

namespace depthxr {

// A game's occlusion predicate can suppress a layer draw, dispatch, copy,
// update or resolve without reporting an error. Temporarily unbind it on
// the immediate context the caller already holds. No wait for the query.
class ScopedD3D11Unpredicated {
  public:
    explicit ScopedD3D11Unpredicated(ID3D11DeviceContext* context) : context_(context) {
        context_->GetPredication(&predicate_, &value_);
        if (predicate_) context_->SetPredication(nullptr, FALSE);
    }
    ~ScopedD3D11Unpredicated() {
        if (predicate_) context_->SetPredication(predicate_.Get(), value_);
    }
    ScopedD3D11Unpredicated(const ScopedD3D11Unpredicated&) = delete;
    ScopedD3D11Unpredicated& operator=(const ScopedD3D11Unpredicated&) = delete;

  private:
    ID3D11DeviceContext* context_;
    Microsoft::WRL::ComPtr<ID3D11Predicate> predicate_;
    BOOL value_{FALSE};
};

} // namespace depthxr
