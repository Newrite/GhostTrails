module;

#include "PCH.h"

export module GhostTrails.Materials;
import GhostTrails.Coverage;

namespace GhostTrails::Materials
{

  using ShaderFlag = RE::BSShaderProperty::EShaderPropertyFlag;

  namespace Address
  {

    constexpr auto AlphaFactory    = REL::VariantID(69311, 70684, 0xCADF10);
    constexpr auto LightingFactory = REL::VariantID(99847, 106492, 0x1302ED0);

  }

  RE::NiPointer<RE::BSLightingShaderProperty> CreateProperty()
  {
    static const REL::Relocation<RE::BSLightingShaderProperty*()> create{Address::LightingFactory};
    return RE::NiPointer<RE::BSLightingShaderProperty>{create()};
  }

  export struct Surface
  {
    // Pointers borrow the ghost's owned scene graph.
    // Shader/material ownership never changes after a successful build.
    RE::BSLightingShaderProperty*     property;
    RE::BSLightingShaderMaterialBase* material;
    RE::NiColor*                      emissive;
    RE::NiColor*                      hairTint;  // Optional borrow: only native HairTint materials have this field.

    void ApplyColor(const RE::NiColor& color) const
    {
      *emissive = color;
      if (hairTint) *hairTint = color;
    }
  };

  export enum class SurfaceError
  {
    MissingMaterial,
    SharedMaterial,
    Allocation,
    FadeControl,
    SetupFailed,
    MissingRimTexture
  };

  export const char* Describe(SurfaceError error)
  {
    switch (error)
    {
      case SurfaceError::MissingMaterial:
        return "lighting/effect material unavailable";
      case SurfaceError::SharedMaterial:
        return "native material copy aliases a source/default material";
      case SurfaceError::Allocation:
        return "material or property allocation failed";
      case SurfaceError::FadeControl:
        return "native empty fade-node prototype could not be cloned independently";
      case SurfaceError::SetupFailed:
        return "native geometry setup failed";
      case SurfaceError::MissingRimTexture:
        return "native geometry setup left the rim texture empty";
    }
    return "unknown surface error";
  }

  export std::expected<RE::NiPointer<RE::BSFadeNode>, SurfaceError> CreateFadeControl()
  {
    const auto property = CreateProperty();
    if (!property) return std::unexpected(SurfaceError::Allocation);
    auto* prototype = property->fadeNode;
    if (!prototype || prototype->GetChildren().size() != 0 || prototype->controllers)
    {
      return std::unexpected(SurfaceError::FadeControl);
    }

    RE::NiCloningProcess process{};
    process.copyType = 1;
    process.scale    = {1, 1, 1};
    RE::NiPointer<RE::NiObject> clone{prototype->CreateClone(process)};
    if (clone) prototype->ProcessClone(process);
    auto* node = netimmerse_cast<RE::BSFadeNode*>(clone.get());
    if (!node || node == prototype) return std::unexpected(SurfaceError::FadeControl);

    auto& state       = node->GetRuntimeData();
    state.currentFade = 1;
    state.unk14C      = 1;
    // Audited on SE/AE (+0x154) and VR (+0x17C): this byte selects
    // alternative/dither fade. Zero selects ordinary depth + colour fade.
    // The controller stays detached, so scene traversal cannot update it.
    state.unk154 = 0;
    return RE::NiPointer<RE::BSFadeNode>{node};
  }

  export std::expected<Surface, SurfaceError> MakeSurface(
    RE::BSGeometry&                           source,
    RE::BSGeometry&                           copy,
    const RE::NiPointer<RE::NiSourceTexture>& white,
    RE::BSFadeNode&                           fade)
  {
    auto* original = netimmerse_cast<RE::BSLightingShaderProperty*>(source.GetGeometryRuntimeData().shaderProperty.get());
    auto* effect   = netimmerse_cast<RE::BSEffectShaderProperty*>(source.GetGeometryRuntimeData().shaderProperty.get());
    if ((!original || !original->material) && (!effect || !effect->GetMaterial()))
    {
      return std::unexpected(SurfaceError::MissingMaterial);
    }

    auto property = CreateProperty();
    if (!property || !property->material) return std::unexpected(SurfaceError::Allocation);
    property->fadeNode = &fade;  // Ghost owns the detached controller beyond the cloned graph.

    // Lit meshes keep their native material subclass. Effect-shaded armor
    // keeps its geometry/skin but uses the factory's ordinary lit material.
    auto* base      = static_cast<RE::BSLightingShaderMaterialBase*>(original ? original->material : property->material);
    auto* temporary = static_cast<RE::BSLightingShaderMaterialBase*>(base->Create());
    if (!temporary) return std::unexpected(SurfaceError::Allocation);
    temporary->CopyMembers(base);
    property->SetMaterial(temporary, true);
    temporary->~BSLightingShaderMaterialBase();
    RE::free(temporary);

    auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(property->material);
    if (!material) return std::unexpected(SurfaceError::MissingMaterial);
    if (material == base || (original && material == original->material))
    {
      return std::unexpected(SurfaceError::SharedMaterial);
    }
    const auto& originalData = source.GetGeometryRuntimeData();
    const bool  textureCoverage =
      originalData.alphaProperty && (originalData.alphaProperty->GetAlphaTesting() || originalData.alphaProperty->GetAlphaBlending());
    // Preserve authored texture alpha for hair/cards. Colour normalization
    // must not erase the cutout, and does not justify a separate GPU path.
    if (textureCoverage)
    {
      if (effect) material->diffuseTexture = effect->GetMaterial()->sourceTexture;
    }
    else if (white)
    {
      material->diffuseTexture = white;
    }
    material->materialAlpha = 1;
    if (original) property->flags = original->flags;
    if (effect && effect->flags.all(ShaderFlag::kTwoSided)) property->flags.set(ShaderFlag::kTwoSided);
    if (source.GetGeometryRuntimeData().skinInstance) property->flags.set(ShaderFlag::kSkinned);
    property->flags.reset(
      ShaderFlag::kCastShadows,
      ShaderFlag::kReceiveShadows,
      ShaderFlag::kSpecular,
      ShaderFlag::kDecal,
      ShaderFlag::kDynamicDecal,
      ShaderFlag::kScreendoorAlphaFade);
    // Prepare has no appearance yet. Provision rim resources before native
    // FinishSetupGeometry, even when the source never used rim lighting.
    // Enabling it only in Spawn leaves material+0x60 null for the renderer.
    property->flags
      .set(ShaderFlag::kZBufferTest, ShaderFlag::kZBufferWrite, ShaderFlag::kNoFade, ShaderFlag::kOwnEmit, ShaderFlag::kRimLighting);
    property->alpha = 1;
    if (!property->emissiveColor)
    {
      auto* color = RE::malloc<RE::NiColor>();
      if (!color) return std::unexpected(SurfaceError::Allocation);
      property->emissiveColor = new (color) RE::NiColor(1, 1, 1);
    }

    static const REL::Relocation<RE::NiAlphaProperty*()> createAlpha{Address::AlphaFactory};
    RE::NiPointer<RE::NiAlphaProperty>                   alpha{createAlpha()};
    if (!alpha) return std::unexpected(SurfaceError::Allocation);
    if (originalData.alphaProperty)
    {
      alpha->alphaFlags = originalData.alphaProperty->alphaFlags;
    }
    alpha->SetAlphaTesting(textureCoverage);
    // A fixed positive or fade-scaled threshold makes depth and colour
    // coverage disagree: only colour includes the fade multiplier. Keep
    // soft strands and let native Utility reject empty texture pixels.
    alpha->alphaThreshold                    = Coverage::silhouetteCutoutReference;
    constexpr std::uint16_t testFunctionMask = 7u << 10;
    alpha->alphaFlags =
      (alpha->alphaFlags & ~testFunctionMask) | (static_cast<std::uint16_t>(RE::NiAlphaProperty::TestFunction::kGreaterEqual) << 10);
    // Native object fade emits a depth prepass only without this bit.
    // The engine enables colour blending from the fade node's opacity.
    alpha->SetAlphaBlending(false);
    alpha->SetSrcBlendMode(RE::NiAlphaProperty::AlphaFunction::kSrcAlpha);
    alpha->SetDestBlendMode(RE::NiAlphaProperty::AlphaFunction::kInvSrcAlpha);

    auto& data          = copy.GetGeometryRuntimeData();
    data.alphaProperty  = alpha;
    data.shaderProperty = property;
    if (!property->SetupGeometry(&copy))
    {
      return std::unexpected(SurfaceError::SetupFailed);
    }
    // The VS otherwise forwards authored vertex alpha to the pixel shader,
    // multiplying the requested ghost alpha once more on some hair/boots.
    property->flags.reset(ShaderFlag::kVertexColors, ShaderFlag::kVertexAlpha);
    if (!property->FinishSetupGeometry(&copy))
    {
      return std::unexpected(SurfaceError::SetupFailed);
    }
    if (!material->rimSoftLightingTexture) return std::unexpected(SurfaceError::MissingRimTexture);
#ifdef GHOSTTRAILS_DIAGNOSTICS
    logger::debug(
      "Ghost material '{}': feature={} authoredAlphaFlags={:04X} authoredThreshold={} cutout={} vertexColoursIgnored=true",
      source.name.c_str(),
      static_cast<unsigned>(material->GetFeature()),
      originalData.alphaProperty ? originalData.alphaProperty->alphaFlags : 0,
      originalData.alphaProperty ? originalData.alphaProperty->alphaThreshold : 0,
      textureCoverage);
#endif
    // Same field as NiAVObject::UpdateHairColor, but restricted to this
    // surface's unique cloned material. Never recolour the live actor.
    auto* hairTint = material->GetFeature() == RE::BSShaderMaterial::Feature::kHairTint
                     ? &static_cast<RE::BSLightingShaderMaterialHairTint*>(material)->tintColor
                     : nullptr;
    return Surface{property.get(), material, property->emissiveColor, hairTint};
  }

}
