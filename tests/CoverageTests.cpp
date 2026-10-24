#define NOMINMAX
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

import GhostTrails.Coverage;

using Microsoft::WRL::ComPtr;
namespace Coverage = GhostTrails::Coverage;

namespace
{
    int failures{};

    bool Check(bool condition, const char* contract)
    {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", contract);
            ++failures;
        }
        return condition;
    }

    void VerifyMip(ID3D11Device& device, ID3D11DeviceContext& context, ID3D11Texture2D& actual,
                   UINT mip, const std::vector<std::uint32_t>& expected, bool neutral)
    {
        D3D11_TEXTURE2D_DESC description{};
        actual.GetDesc(&description);
        const auto width = std::max(1u, description.Width >> mip);
        const auto height = std::max(1u, description.Height >> mip);
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> readback;
        if (!Check(SUCCEEDED(device.CreateTexture2D(&description, nullptr, &readback)), "readback allocation")) return;
        context.CopyResource(readback.Get(), &actual);

        D3D11_MAPPED_SUBRESOURCE pixels{};
        if (!Check(SUCCEEDED(context.Map(readback.Get(), mip, D3D11_MAP_READ, 0, &pixels)), "actual GPU completion/readback")) return;
        bool equal = true;
        for (UINT y = 0; y < height; ++y) {
            const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const std::byte*>(pixels.pData) + y * pixels.RowPitch);
            for (UINT x = 0; x < width; ++x) {
                const auto original = expected[y * width + x];
                const auto wanted = neutral ? (original | 0x00FFFFFFu) : original;
                equal &= row[x] == wanted;
            }
        }
        context.Unmap(readback.Get(), mip);
        Check(equal, neutral ? "every texel is white RGB with the exact original alpha, including dispatch edges" : "source texture unchanged");
    }
}

int main(int argc, char** argv)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    const auto driver = argc > 1 && std::string_view(argv[1]) == "--hardware"
        ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
    if (!Check(SUCCEEDED(D3D11CreateDevice(nullptr, driver, nullptr, 0, &level, 1,
        D3D11_SDK_VERSION, &device, nullptr, &context)), "real D3D11 device initialization")) return 1;

    constexpr UINT width = 17, height = 9, mips = 3;
    Coverage::Budget budget{32u * 1024 * 1024};
    std::array<std::vector<std::uint32_t>, mips> original;
    std::array<D3D11_SUBRESOURCE_DATA, mips> input{};
    for (UINT mip = 0; mip < mips; ++mip) {
        const auto w = std::max(1u, width >> mip), h = std::max(1u, height >> mip);
        original[mip].resize(w * h);
        for (UINT i = 0; i < w * h; ++i) original[mip][i] = (((i * 17 + mip * 73) % 256) << 24) | 0x00372516u;
        input[mip] = {original[mip].data(), w * 4, 0};
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = mips;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> source;
    if (!Check(SUCCEEDED(device->CreateTexture2D(&description, input.data(), &source)), "source allocation")) return 1;
    ComPtr<ID3D11ShaderResourceView> view;
    if (!Check(SUCCEEDED(device->CreateShaderResourceView(source.Get(), nullptr, &view)), "source view")) return 1;

    auto* originalView = view.Get();
    context->CSSetShaderResources(0, 1, &originalView);
    const D3D11_VIEWPORT viewport{3, 4, 71, 39, 0.2f, 0.9f};
    context->RSSetViewports(1, &viewport);
    auto converted = Coverage::Neutralize(*device.Get(), *context.Get(), *view.Get(), budget);
    if (!Check(converted.has_value(), "production coverage converter succeeds")) return 1;
    Check(converted->width == width && converted->height == height && converted->mips == mips, "all source mip dimensions retained");
    ComPtr<ID3D11ShaderResourceView> restored;
    context->CSGetShaderResources(0, 1, &restored);
    Check(restored.Get() == view.Get(), "immediate compute bindings restored");
    UINT viewports = 1;
    D3D11_VIEWPORT after{};
    context->RSGetViewports(&viewports, &after);
    Check(viewports == 1 && after.TopLeftX == viewport.TopLeftX && after.Width == viewport.Width &&
        after.MinDepth == viewport.MinDepth && after.MaxDepth == viewport.MaxDepth, "graphics pipeline state restored");
    for (UINT mip = 0; mip < mips; ++mip) {
        VerifyMip(*device.Get(), *context.Get(), *converted->pixels.Get(), mip, original[mip], true);
        VerifyMip(*device.Get(), *context.Get(), *source.Get(), mip, original[mip], false);
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC subset{};
    view->GetDesc(&subset);
    subset.Texture2D.MostDetailedMip = 1;
    subset.Texture2D.MipLevels = 2;
    ComPtr<ID3D11ShaderResourceView> subsetView;
    if (!Check(SUCCEEDED(device->CreateShaderResourceView(source.Get(), &subset, &subsetView)), "mip subset view")) return 1;
    auto subsetTexture = Coverage::Neutralize(*device.Get(), *context.Get(), *subsetView.Get(), budget);
    if (Check(subsetTexture.has_value(), "source views starting at a nonzero mip supported")) {
        Check(subsetTexture->width == 8 && subsetTexture->height == 4 && subsetTexture->mips == 2, "mip subset output dimensions");
        VerifyMip(*device.Get(), *context.Get(), *subsetTexture->pixels.Get(), 0, original[1], true);
        VerifyMip(*device.Get(), *context.Get(), *subsetTexture->pixels.Get(), 1, original[2], true);
    }

    ComPtr<ID3D11DeviceContext> deferred;
    if (Check(SUCCEEDED(device->CreateDeferredContext(0, &deferred)), "deferred context fixture")) {
        const auto rejected = Coverage::Neutralize(*device.Get(), *deferred.Get(), *view.Get(), budget);
        Check(!rejected && rejected.error().cause == Coverage::Failure::UnsupportedTexture,
            "reject a non-immediate submission context with a typed failure");
    }

    // A compressed sRGB source exercises the path actually used by DDS hair.
    // One black BC3 block with uniform 128 alpha has an unambiguous GPU decode.
    std::array<std::uint8_t, 16> block{};
    block[0] = 128;
    auto compressedDescription = description;
    compressedDescription.Width = compressedDescription.Height = 4;
    compressedDescription.MipLevels = 1;
    compressedDescription.Format = DXGI_FORMAT_BC3_UNORM_SRGB;
    const D3D11_SUBRESOURCE_DATA compressedData{block.data(), 16, 0};
    ComPtr<ID3D11Texture2D> compressed;
    ComPtr<ID3D11ShaderResourceView> compressedView;
    if (Check(SUCCEEDED(device->CreateTexture2D(&compressedDescription, &compressedData, &compressed)), "compressed source")) {
        if (Check(SUCCEEDED(device->CreateShaderResourceView(compressed.Get(), nullptr, &compressedView)), "compressed view")) {
            auto masked = Coverage::Neutralize(*device.Get(), *context.Get(), *compressedView.Get(), budget);
            if (Check(masked.has_value(), "compressed sRGB DDS coverage conversion")) {
                VerifyMip(*device.Get(), *context.Get(), *masked->pixels.Get(), 0, std::vector<std::uint32_t>(16, 0x80000000u), true);
            }
        }
    }
    {
        Coverage::Budget constrained{1};
        auto rejected = Coverage::Neutralize(*device.Get(), *context.Get(), *view.Get(), constrained);
        Check(!rejected && rejected.error().cause == Coverage::Failure::Capacity, "GPU allocation is rejected before exceeding the shared byte budget");
    }
    {
        Coverage::Budget constrained{64};
        {
            auto first = constrained.Acquire(48);
            Check(first.has_value(), "admit within memory budget");
            auto denied = constrained.Acquire(17);
            Check(!denied && denied.error().cause == Coverage::Failure::Capacity, "all retained masks share admission");
            auto moved = std::move(first);
            Check(!constrained.Acquire(17), "moving ownership does not release its GPU budget");
        }
        Check(constrained.Acquire(64).has_value(), "resource destruction returns the entire budget");
    }
    if (failures) return 1;
    std::puts("GPU coverage, authored alpha, mip ranges, source isolation and pipeline restoration passed");
    return 0;
}
