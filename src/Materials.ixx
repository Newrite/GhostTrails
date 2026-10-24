module;

#include "PCH.h"

export module GhostTrails.Materials;
import GhostTrails.Coverage;

namespace GhostTrails::Materials
{
    using ShaderFlag = RE::BSShaderProperty::EShaderPropertyFlag;

    namespace Address
    {
        constexpr auto AlphaFactory = REL::VariantID(69311, 70684, 0xCADF10);
        constexpr auto LightingFactory = REL::VariantID(99847, 106492, 0x1302ED0);
    }

    export struct Surface
    {
        // Pointers borrow the ghost's owned scene graph.
        // Shader/material ownership never changes after a successful build.
        RE::BSLightingShaderProperty* property;
        RE::BSLightingShaderMaterialBase* material;
        RE::NiColor* emissive;
        RE::NiAlphaProperty* alpha;
        std::optional<Coverage::Cutout> cutout;

        void ApplyOpacity(float opacity) const
        {
            material->materialAlpha = opacity;
            if (cutout) alpha->alphaThreshold = cutout->Reference(opacity);
        }
    };

    export enum class SurfaceError { MissingMaterial, SharedMaterial, Allocation, SetupFailed, MissingRimTexture };

    export const char* Describe(SurfaceError error)
    {
        switch (error) {
        case SurfaceError::MissingMaterial: return "lighting/effect material unavailable";
        case SurfaceError::SharedMaterial: return "native material copy aliases a source/default material";
        case SurfaceError::Allocation: return "material or property allocation failed";
        case SurfaceError::SetupFailed: return "native geometry setup failed";
        case SurfaceError::MissingRimTexture: return "native geometry setup left the rim texture empty";
        }
        return "unknown surface error";
    }

    export std::expected<Surface, SurfaceError> MakeSurface(RE::BSGeometry& source, RE::BSGeometry& copy,
                                                    const RE::NiPointer<RE::NiSourceTexture>& white)
    {
        auto* original = netimmerse_cast<RE::BSLightingShaderProperty*>(source.GetGeometryRuntimeData().shaderProperty.get());
        auto* effect = netimmerse_cast<RE::BSEffectShaderProperty*>(source.GetGeometryRuntimeData().shaderProperty.get());
        if ((!original || !original->material) && (!effect || !effect->GetMaterial())) {
            return std::unexpected(SurfaceError::MissingMaterial);
        }

        static const REL::Relocation<RE::BSLightingShaderProperty*()> create{Address::LightingFactory};
        RE::NiPointer<RE::BSLightingShaderProperty> property{create()};
        if (!property || !property->material) return std::unexpected(SurfaceError::Allocation);

        // Lit meshes keep their native material subclass. Effect-shaded armor
        // keeps its geometry/skin but uses the factory's ordinary lit material.
        auto* base = static_cast<RE::BSLightingShaderMaterialBase*>(original ? original->material : property->material);
        auto* temporary = static_cast<RE::BSLightingShaderMaterialBase*>(base->Create());
        if (!temporary) return std::unexpected(SurfaceError::Allocation);
        temporary->CopyMembers(base);
        property->SetMaterial(temporary, true);
        temporary->~BSLightingShaderMaterialBase();
        RE::free(temporary);

        auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(property->material);
        if (!material) return std::unexpected(SurfaceError::MissingMaterial);
        if (material == base || (original && material == original->material)) {
            return std::unexpected(SurfaceError::SharedMaterial);
        }
        const auto& originalData = source.GetGeometryRuntimeData();
        const bool textureCoverage = originalData.alphaProperty &&
            (originalData.alphaProperty->GetAlphaTesting() || originalData.alphaProperty->GetAlphaBlending());
        // Preserve authored texture alpha for hair/cards. Colour normalization
        // must not erase the cutout, and does not justify a separate GPU path.
        if (textureCoverage) {
            if (effect) material->diffuseTexture = effect->GetMaterial()->sourceTexture;
        } else if (white) {
            material->diffuseTexture = white;
        }
        material->materialAlpha = 1;
        if (original) property->flags = original->flags;
        if (effect && effect->flags.all(ShaderFlag::kTwoSided)) property->flags.set(ShaderFlag::kTwoSided);
        if (source.GetGeometryRuntimeData().skinInstance) property->flags.set(ShaderFlag::kSkinned);
        property->flags.reset(ShaderFlag::kCastShadows, ShaderFlag::kReceiveShadows, ShaderFlag::kSpecular,
                              ShaderFlag::kDecal, ShaderFlag::kDynamicDecal, ShaderFlag::kScreendoorAlphaFade);
        // Prepare has no appearance yet. Provision rim resources before native
        // FinishSetupGeometry, even when the source never used rim lighting.
        // Enabling it only in Spawn leaves material+0x60 null for the renderer.
        property->flags.set(ShaderFlag::kZBufferTest, ShaderFlag::kZBufferWrite, ShaderFlag::kNoFade,
                            ShaderFlag::kOwnEmit, ShaderFlag::kRimLighting);
        property->alpha = 1;
        if (!property->emissiveColor) {
            auto* color = RE::malloc<RE::NiColor>();
            if (!color) return std::unexpected(SurfaceError::Allocation);
            property->emissiveColor = new (color) RE::NiColor(1, 1, 1);
        }

        static const REL::Relocation<RE::NiAlphaProperty*()> createAlpha{Address::AlphaFactory};
        RE::NiPointer<RE::NiAlphaProperty> alpha{createAlpha()};
        if (!alpha) return std::unexpected(SurfaceError::Allocation);
        std::optional<Coverage::Cutout> cutout;
        if (originalData.alphaProperty) {
            alpha->alphaFlags = originalData.alphaProperty->alphaFlags;
            alpha->alphaThreshold = originalData.alphaProperty->alphaThreshold;
            if (originalData.alphaProperty->GetAlphaTesting()) cutout.emplace(alpha->alphaThreshold);
        } else {
            alpha->SetAlphaTesting(false);
        }
        alpha->SetAlphaBlending(true);
        alpha->SetSrcBlendMode(RE::NiAlphaProperty::AlphaFunction::kSrcAlpha);
        // Ordinary alpha composition avoids adding every head/body layer's
        // full brightness over the scene, which exaggerated their difference.
        alpha->SetDestBlendMode(RE::NiAlphaProperty::AlphaFunction::kInvSrcAlpha);

        auto& data = copy.GetGeometryRuntimeData();
        data.alphaProperty = alpha;
        data.shaderProperty = property;
        if (!property->SetupGeometry(&copy)) {
            return std::unexpected(SurfaceError::SetupFailed);
        }
        // The VS otherwise forwards authored vertex alpha to the pixel shader,
        // multiplying the requested ghost alpha once more on some hair/boots.
        property->flags.reset(ShaderFlag::kVertexColors, ShaderFlag::kVertexAlpha);
        if (!property->FinishSetupGeometry(&copy)) {
            return std::unexpected(SurfaceError::SetupFailed);
        }
        if (!material->rimSoftLightingTexture) return std::unexpected(SurfaceError::MissingRimTexture);
#ifdef GHOSTTRAILS_DIAGNOSTICS
        logger::debug("Ghost material '{}': feature={} authoredAlphaFlags={:04X} authoredThreshold={} cutout={} vertexColoursIgnored=true",
            source.name.c_str(), static_cast<unsigned>(material->GetFeature()),
            originalData.alphaProperty ? originalData.alphaProperty->alphaFlags : 0,
            originalData.alphaProperty ? originalData.alphaProperty->alphaThreshold : 0, cutout.has_value());
#endif
        return Surface{property.get(), material, property->emissiveColor, alpha.get(), cutout};
    }

}
