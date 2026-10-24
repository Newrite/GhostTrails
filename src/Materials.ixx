module;

#include "PCH.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <unordered_set>

export module GhostTrails.Materials;
import GhostTrails.Coverage;

namespace GhostTrails::Materials
{
    export using Budget = Coverage::Budget;
    using ShaderFlag = RE::BSShaderProperty::EShaderPropertyFlag;

    namespace Address
    {
        constexpr auto AlphaFactory = REL::VariantID(69311, 70684, 0xCADF10);
        constexpr auto LightingFactory = REL::VariantID(99847, 106492, 0x1302ED0);
        constexpr auto TextureFactory = REL::VariantID(69335, 70717, 0xCAEF60);
    }

    export struct Surface
    {
        // All three pointers are borrows from the ghost's owned scene graph.
        // Shader/material ownership never changes after a successful build.
        RE::BSLightingShaderProperty* property;
        RE::BSLightingShaderMaterialBase* material;
        RE::NiColor* emissive;
    };

    export enum class SurfaceError
    {
        MissingMaterial, Allocation, SetupFailed, MissingRimTexture,
        MissingCoverageTexture, RendererUnavailable, Coverage, FadeControl
    };

    export const char* Describe(SurfaceError error)
    {
        switch (error) {
        case SurfaceError::MissingMaterial: return "lighting/effect material unavailable";
        case SurfaceError::Allocation: return "material or property allocation failed";
        case SurfaceError::SetupFailed: return "native geometry setup failed";
        case SurfaceError::MissingRimTexture: return "native geometry setup left the rim texture empty";
        case SurfaceError::MissingCoverageTexture: return "cutout diffuse texture/SRV is not ready";
        case SurfaceError::RendererUnavailable: return "D3D11 renderer/device is unavailable";
        case SurfaceError::Coverage: return "cutout texture conversion failed";
        case SurfaceError::FadeControl: return "native fade controller could not be cloned";
        }
        return "unknown surface error";
    }

    // At most one entry per cutout texture in this pool's retained source graph.
    // Native materials own the resulting masks; Clear/3D changes/idle eviction
    // release the cache with the actor pool, independently of individual ghosts.
    export class Masks
    {
    public:
        explicit Masks(Budget& budget) : budget_(budget) {}

        void Clear() { entries_.clear(); }

        void Retain(std::span<RE::NiAVObject* const> objects)
        {
            std::unordered_set<RE::NiSourceTexture*> used;
            for (auto* object : objects) {
                const auto* geometry = object->AsGeometry();
                if (!geometry) continue;
                auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
                if (const auto* lit = netimmerse_cast<RE::BSLightingShaderProperty*>(property); lit && lit->material) {
                    used.insert(static_cast<RE::BSLightingShaderMaterialBase*>(lit->material)->diffuseTexture.get());
                } else if (auto* effect = netimmerse_cast<RE::BSEffectShaderProperty*>(property); effect && effect->GetMaterial()) {
                    used.insert(effect->GetMaterial()->sourceTexture.get());
                }
            }
            std::erase_if(entries_, [&](const auto& entry) { return !used.contains(entry.first); });
        }

        std::expected<RE::NiPointer<RE::NiSourceTexture>, SurfaceError> Get(RE::NiSourceTexture* source)
        {
            if (!source || !source->rendererTexture || !source->rendererTexture->resourceView) {
                return std::unexpected(SurfaceError::MissingCoverageTexture);
            }
            const auto found = entries_.find(source);
            if (found != entries_.end() && found->second.view == source->rendererTexture->resourceView) {
                return found->second.mask;
            }
            // Matches invalidates old ghosts when a source SRV is replaced.
            // Return its stale cache allocation before admitting a replacement.
            if (found != entries_.end()) entries_.erase(found);

            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            if (!renderer) return std::unexpected(SurfaceError::RendererUnavailable);
            struct Lock
            {
                RE::BSGraphics::Renderer& renderer;
                explicit Lock(RE::BSGraphics::Renderer& value) : renderer(value) { renderer.Lock(); }
                ~Lock() { renderer.Unlock(); }
            };

            // Native and SDK COM interfaces have the same Windows ABI. Keep
            // that interoperability cast at this renderer adapter boundary.
            auto* device = reinterpret_cast<ID3D11Device*>(RE::BSGraphics::Renderer::GetDevice());
            if (!device) return std::unexpected(SurfaceError::RendererUnavailable);
            Coverage::ComPtr<ID3D11DeviceContext> context;
            device->GetImmediateContext(&context);
            auto converted = [&] {
                Lock lock{*renderer};
                auto* input = reinterpret_cast<ID3D11ShaderResourceView*>(source->rendererTexture->resourceView);
                return Coverage::Neutralize(*device, *context.Get(), *input, budget_);
            }();
            if (!converted) {
                logger::warn("Ghost cutout '{}' failed: stage={} HRESULT={:08X}", source->name.c_str(),
                    Coverage::Describe(converted.error().cause), static_cast<unsigned>(converted.error().result));
                return std::unexpected(SurfaceError::Coverage);
            }

            static const REL::Relocation<RE::NiSourceTexture*(const RE::BSFixedString&)> create{Address::TextureFactory};
            RE::NiPointer<RE::NiSourceTexture> mask{create(RE::BSFixedString("GhostTrails coverage"))};
            if (!mask) return std::unexpected(SurfaceError::Allocation);
            mask->rendererTexture = nullptr;
            auto* data = RE::malloc<RE::BSGraphics::Texture>();
            if (!data) return std::unexpected(SurfaceError::Allocation);
            *data = {};
            data->texture = reinterpret_cast<REX::W32::ID3D11Resource*>(converted->pixels.Detach());
            data->resourceView = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(converted->view.Detach());
            data->width = static_cast<std::uint16_t>(converted->width);
            data->height = static_cast<std::uint16_t>(converted->height);
            data->mips = static_cast<std::uint8_t>(converted->mips);
            data->format = 0x1C; // DXGI_FORMAT_R8G8B8A8_UNORM.
            data->refCount = 1;
            mask->rendererTexture = data;
            entries_.insert_or_assign(source, Entry{std::move(converted->memory),
                RE::NiPointer<RE::NiSourceTexture>(source), mask, source->rendererTexture->resourceView});
#ifdef GHOSTTRAILS_DIAGNOSTICS
            logger::debug("Ghost cutout '{}' cached: {}x{} mips={} textureCount={}", source->name.c_str(),
                converted->width, converted->height, converted->mips, entries_.size());
#endif
            return mask;
        }

    private:
        struct Entry
        {
            Budget::Lease memory;
            RE::NiPointer<RE::NiSourceTexture> source, mask;
            const void* view;
        };

        Budget& budget_;
        std::unordered_map<RE::NiSourceTexture*, Entry> entries_;
    };

    export std::expected<Surface, SurfaceError> MakeSurface(RE::BSGeometry& source, RE::BSGeometry& copy,
        const RE::NiPointer<RE::NiSourceTexture>& white, Masks& masks, RE::NiPointer<RE::BSFadeNode>& fade)
    {
        auto* original = netimmerse_cast<RE::BSLightingShaderProperty*>(source.GetGeometryRuntimeData().shaderProperty.get());
        auto* effect = netimmerse_cast<RE::BSEffectShaderProperty*>(source.GetGeometryRuntimeData().shaderProperty.get());
        if ((!original || !original->material) && (!effect || !effect->GetMaterial())) {
            return std::unexpected(SurfaceError::MissingMaterial);
        }

        static const REL::Relocation<RE::BSLightingShaderProperty*()> create{Address::LightingFactory};
        RE::NiPointer<RE::BSLightingShaderProperty> property{create()};
        if (!property || !property->material) return std::unexpected(SurfaceError::Allocation);

        if (!fade) {
            if (!property->fadeNode) return std::unexpected(SurfaceError::FadeControl);
            // The factory supplies the engine's empty fade-node prototype.
            // Clone it rather than borrow a node the scene can update or unload.
            RE::NiCloningProcess process{};
            process.copyType = 1;
            process.scale = {1, 1, 1};
            RE::NiPointer<RE::NiObject> controller{property->fadeNode->CreateClone(process)};
            if (controller) property->fadeNode->ProcessClone(process);
            auto* node = netimmerse_cast<RE::BSFadeNode*>(controller.get());
            if (!node || node == property->fadeNode) return std::unexpected(SurfaceError::FadeControl);
            fade.reset(node);
        }
        property->fadeNode = fade.get(); // Borrow; the Ghost owns it beyond its scene root.

        // A single ordinary lighting variant for skin, hair, eyes and clothing.
        // No authored tint, normal/glow map, vertex colour or per-part opacity.
        auto* base = static_cast<RE::BSLightingShaderMaterialBase*>(property->material);
        auto* temporary = static_cast<RE::BSLightingShaderMaterialBase*>(base->Create());
        if (!temporary) return std::unexpected(SurfaceError::Allocation);
        temporary->CopyMembers(base);
        property->SetMaterial(temporary, true);
        temporary->~BSLightingShaderMaterialBase();
        RE::free(temporary);

        auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(property->material);
        if (!material) return std::unexpected(SurfaceError::MissingMaterial);
        material->materialAlpha = 1;
        material->diffuseTexture = white;
        const auto& originalData = source.GetGeometryRuntimeData();
        const bool cutout = originalData.alphaProperty &&
            (originalData.alphaProperty->GetAlphaTesting() || originalData.alphaProperty->GetAlphaBlending());
        if (cutout) {
            auto* sourceMaterial = original ? static_cast<RE::BSLightingShaderMaterialBase*>(original->material) : nullptr;
            auto texture = masks.Get(sourceMaterial ? sourceMaterial->diffuseTexture.get() : effect->GetMaterial()->sourceTexture.get());
            if (!texture) return std::unexpected(texture.error());
            material->diffuseTexture = *texture;
        }
        const auto* sourceMaterial = original ? original->material : effect->GetMaterial();
        std::copy(std::begin(sourceMaterial->texCoordOffset), std::end(sourceMaterial->texCoordOffset), material->texCoordOffset);
        std::copy(std::begin(sourceMaterial->texCoordScale), std::end(sourceMaterial->texCoordScale), material->texCoordScale);
        material->textureClampMode = original
            ? static_cast<const RE::BSLightingShaderMaterialBase*>(sourceMaterial)->textureClampMode
            : static_cast<std::int32_t>(effect->GetMaterial()->effectClampMode.underlying());
        const auto* sourceProperty = original ? static_cast<RE::BSShaderProperty*>(original) : effect;
        property->flags.set(sourceProperty->flags.all(ShaderFlag::kTwoSided), ShaderFlag::kTwoSided);
        property->flags.set(sourceProperty->flags.all(ShaderFlag::kModelSpaceNormals), ShaderFlag::kModelSpaceNormals);
        if (source.GetGeometryRuntimeData().skinInstance) property->flags.set(ShaderFlag::kSkinned);
        property->flags.reset(ShaderFlag::kCastShadows, ShaderFlag::kReceiveShadows, ShaderFlag::kSpecular);
        // Prepare has no appearance yet. Provision rim resources before native
        // FinishSetupGeometry, even when the source never used rim lighting.
        // Enabling it only in Spawn leaves material+0x60 null for the renderer.
        property->flags.set(ShaderFlag::kZBufferTest, ShaderFlag::kZBufferWrite, ShaderFlag::kNoFade,
                            ShaderFlag::kOwnEmit, ShaderFlag::kRimLighting, ShaderFlag::kScreendoorAlphaFade);
        property->alpha = 1;
        if (!property->emissiveColor) {
            auto* color = RE::malloc<RE::NiColor>();
            if (!color) return std::unexpected(SurfaceError::Allocation);
            property->emissiveColor = new (color) RE::NiColor(1, 1, 1);
        }

        static const REL::Relocation<RE::NiAlphaProperty*()> createAlpha{Address::AlphaFactory};
        RE::NiPointer<RE::NiAlphaProperty> alpha{createAlpha()};
        if (!alpha) return std::unexpected(SurfaceError::Allocation);
        alpha->SetAlphaBlending(false);
        alpha->SetAlphaTesting(cutout);
        // Blended-only hair has no authored test: keep any nonzero coverage.
        constexpr std::uint8_t nonzeroCoverage = 1;
        alpha->alphaThreshold = cutout && originalData.alphaProperty->GetAlphaTesting()
            ? originalData.alphaProperty->alphaThreshold : nonzeroCoverage;
        constexpr std::uint16_t testFunctionMask = 7u << 10;
        const auto comparison = cutout && originalData.alphaProperty->GetAlphaTesting()
            ? originalData.alphaProperty->alphaFlags & testFunctionMask
            : static_cast<std::uint16_t>(RE::NiAlphaProperty::TestFunction::kGreaterEqual) << 10;
        alpha->alphaFlags = (alpha->alphaFlags & ~testFunctionMask) |
            comparison;

        auto& data = copy.GetGeometryRuntimeData();
        data.alphaProperty = alpha;
        data.shaderProperty = property;
        if (!property->SetupGeometry(&copy)) {
            return std::unexpected(SurfaceError::SetupFailed);
        }
        // SetupGeometry derives this flag from the vertex stream. The stream
        // remains untouched, but the silhouette must ignore its colour/alpha.
        property->flags.reset(ShaderFlag::kVertexColors, ShaderFlag::kVertexAlpha);
        if (!property->FinishSetupGeometry(&copy)) {
            return std::unexpected(SurfaceError::SetupFailed);
        }
        if (!material->rimSoftLightingTexture) return std::unexpected(SurfaceError::MissingRimTexture);
        return Surface{property.get(), material, property->emissiveColor};
    }

}
