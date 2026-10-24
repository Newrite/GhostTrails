module;

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <expected>
#include <utility>
#include "../shaders/Coverage.bytecode.h"

export module GhostTrails.Coverage;

export namespace GhostTrails::Coverage
{
    using Microsoft::WRL::ComPtr;

    enum class Failure { UnsupportedTexture, Resource, Shader, CommandList, Capacity };

    const char* Describe(Failure failure)
    {
        switch (failure) {
        case Failure::UnsupportedTexture: return "unsupported texture/view or submission context";
        case Failure::Resource: return "GPU texture/view allocation";
        case Failure::Shader: return "compute shader creation";
        case Failure::CommandList: return "GPU command recording";
        case Failure::Capacity: return "MaxCoverageMiB budget exhausted";
        }
        return "unknown coverage failure";
    }

    struct Error
    {
        Failure cause;
        HRESULT result;
    };

    // Serialized by the game-thread renderer owner. It must outlive every lease.
    class Budget
    {
    public:
        class Lease
        {
        public:
            Lease(const Lease&) = delete;
            Lease& operator=(const Lease&) = delete;
            Lease(Lease&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)), bytes_(other.bytes_) {}

            Lease& operator=(Lease&& other) noexcept
            {
                if (this != &other) {
                    Release();
                    owner_ = std::exchange(other.owner_, nullptr);
                    bytes_ = other.bytes_;
                }
                return *this;
            }

            ~Lease() { Release(); }

        private:
            friend class Budget;
            Lease(Budget& owner, std::uint64_t bytes) : owner_(&owner), bytes_(bytes) {}

            void Release()
            {
                if (owner_) owner_->used_ -= bytes_;
                owner_ = nullptr;
            }

            Budget* owner_;
            std::uint64_t bytes_;
        };

        explicit Budget(std::uint64_t bytes) : limit_(bytes) {}

        Budget(const Budget&) = delete;
        Budget& operator=(const Budget&) = delete;

        std::expected<Lease, Error> Acquire(std::uint64_t bytes)
        {
            if (bytes > limit_ - used_) return std::unexpected(Error{Failure::Capacity, E_OUTOFMEMORY});
            used_ += bytes;
            return Lease(*this, bytes);
        }

    private:
        std::uint64_t limit_, used_{};
    };

    struct Texture
    {
        Budget::Lease memory; // Last released, after GPU resource ownership.
        ComPtr<ID3D11Texture2D> pixels;
        ComPtr<ID3D11ShaderResourceView> view;
        std::uint32_t width, height, mips;
    };

    // Cold path only. The caller serializes access to the immediate context.
    // No readback or GPU wait; the later scene draws follow this command list.
    std::expected<Texture, Error> Neutralize(ID3D11Device& device, ID3D11DeviceContext& context,
                                             ID3D11ShaderResourceView& source, Budget& budget)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sourceView{};
        source.GetDesc(&sourceView);
        if (sourceView.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
            return std::unexpected(Error{Failure::UnsupportedTexture, E_INVALIDARG});
        }

        ComPtr<ID3D11Resource> resource;
        source.GetResource(&resource);
        ComPtr<ID3D11Texture2D> sourceTexture;
        auto result = resource.As(&sourceTexture);
        if (FAILED(result)) return std::unexpected(Error{Failure::UnsupportedTexture, result});

        D3D11_TEXTURE2D_DESC description{};
        sourceTexture->GetDesc(&description);
        const auto first = sourceView.Texture2D.MostDetailedMip;
        if (description.ArraySize != 1 || description.SampleDesc.Count != 1 || first >= description.MipLevels) {
            return std::unexpected(Error{Failure::UnsupportedTexture, E_INVALIDARG});
        }
        const auto count = sourceView.Texture2D.MipLevels == UINT(-1)
            ? description.MipLevels - first : sourceView.Texture2D.MipLevels;
        if (!count || count > description.MipLevels - first || context.GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
            return std::unexpected(Error{Failure::UnsupportedTexture, E_INVALIDARG});
        }

        const auto width = std::max(1u, description.Width >> first);
        const auto height = std::max(1u, description.Height >> first);
        std::uint64_t bytes{};
        for (UINT mip = 0; mip < count; ++mip) {
            bytes += std::uint64_t(std::max(1u, width >> mip)) * std::max(1u, height >> mip) * 4;
        }
        auto memory = budget.Acquire(bytes);
        if (!memory) return std::unexpected(memory.error());
        Texture output{std::move(*memory), {}, {}, width, height, count};
        D3D11_TEXTURE2D_DESC target{};
        target.Width = output.width;
        target.Height = output.height;
        target.MipLevels = count;
        target.ArraySize = 1;
        target.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        target.SampleDesc.Count = 1;
        target.Usage = D3D11_USAGE_DEFAULT;
        target.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        result = device.CreateTexture2D(&target, nullptr, &output.pixels);
        if (FAILED(result)) return std::unexpected(Error{Failure::Resource, result});
        result = device.CreateShaderResourceView(output.pixels.Get(), nullptr, &output.view);
        if (FAILED(result)) return std::unexpected(Error{Failure::Resource, result});

        ComPtr<ID3D11ComputeShader> shader;
        result = device.CreateComputeShader(CoverageShader, sizeof(CoverageShader), nullptr, &shader);
        if (FAILED(result)) return std::unexpected(Error{Failure::Shader, result});
        ComPtr<ID3D11DeviceContext> commands;
        result = device.CreateDeferredContext(0, &commands);
        if (FAILED(result)) return std::unexpected(Error{Failure::CommandList, result});
        commands->CSSetShader(shader.Get(), nullptr, 0);

        for (UINT mip = 0; mip < count; ++mip) {
            auto inputDescription = sourceView;
            inputDescription.Texture2D.MostDetailedMip = first + mip;
            inputDescription.Texture2D.MipLevels = 1;
            ComPtr<ID3D11ShaderResourceView> input;
            result = device.CreateShaderResourceView(resource.Get(), &inputDescription, &input);
            if (FAILED(result)) return std::unexpected(Error{Failure::Resource, result});

            D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = target.Format;
            view.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipSlice = mip;
            ComPtr<ID3D11UnorderedAccessView> destination;
            result = device.CreateUnorderedAccessView(output.pixels.Get(), &view, &destination);
            if (FAILED(result)) return std::unexpected(Error{Failure::Resource, result});

            auto* inputPointer = input.Get();
            auto* targetPointer = destination.Get();
            commands->CSSetShaderResources(0, 1, &inputPointer);
            commands->CSSetUnorderedAccessViews(0, 1, &targetPointer, nullptr);
            commands->Dispatch((std::max(1u, output.width >> mip) + 7) / 8,
                               (std::max(1u, output.height >> mip) + 7) / 8, 1);
        }

        ComPtr<ID3D11CommandList> list;
        result = commands->FinishCommandList(FALSE, &list);
        if (FAILED(result)) return std::unexpected(Error{Failure::CommandList, result});
        // Preserve all engine bindings and pipeline state, including CS/UAVs.
        // https://learn.microsoft.com/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-executecommandlist
        context.ExecuteCommandList(list.Get(), TRUE);
        return output;
    }
}
