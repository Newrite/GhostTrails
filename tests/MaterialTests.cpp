#include <cstdint>
#include <cstdio>
#include <optional>

import GhostTrails.Coverage;

namespace C = GhostTrails::Coverage;

namespace
{

  int failures{};

  void Check(bool condition, const char* contract)
  {
    if (!condition)
    {
      std::fprintf(stderr, "FAIL: %s\n", contract);
      ++failures;
    }
  }

  void SurfaceSelection()
  {
    C::SourceSurface hair{"_BDO__PPW10A_Ba_45", C::Shading::Lighting, C::Skinning::Skinned, C::Role::Decal, 1, 1};
    Check(!C::Exclude(hair), "skinned Dint hair remains eligible despite its decal flag");
    hair.role = C::Role::WeaponBlood;
    Check(C::Exclude(hair) == C::Exclusion::Decal, "dedicated blood is still auxiliary when skinned");
    hair.role     = C::Role::Decal;
    hair.skinning = C::Skinning::Rigid;
    Check(C::Exclude(hair) == C::Exclusion::Decal, "rigid decals are excluded");

    C::SourceSurface effect{"weapon fire", C::Shading::Effect, C::Skinning::Rigid, C::Role::Ordinary, 1, 1};
    Check(C::Exclude(effect) == C::Exclusion::AuxiliaryEffect, "rigid fire/effect geometry is not converted into actor clothing");
    effect.skinning = C::Skinning::Skinned;
    Check(!C::Exclude(effect), "skinned effect-shaded clothing remains eligible");
    effect.propertyAlpha = 0;
    Check(C::Exclude(effect) == C::Exclusion::Invisible, "invisible source properties are excluded");
    effect.propertyAlpha = 1;
    effect.materialAlpha = 0;
    Check(C::Exclude(effect) == C::Exclusion::Invisible, "invisible collider materials are excluded");
    effect.materialAlpha = 1;
    effect.shading       = C::Shading::Unsupported;
    Check(C::Exclude(effect) == C::Exclusion::Unsupported, "unsupported materials are diagnosed");
    effect.name = "Body [SOvl0]";
    Check(C::Exclude(effect) == C::Exclusion::Overlay, "overlay exclusion is independent of material support");
  }

  void VertexFormatCoverage()
  {
    // A rigid surface without a normal renders black under the ghost's fresh lighting
    // material, while an unreadable vertex description proves nothing.
    C::SourceSurface body{"malebody", C::Shading::Lighting, C::Skinning::Rigid, C::Role::Ordinary, 1, 1};
    Check(!C::Exclude(body), "an unreadable vertex description never excludes a surface");

    body.normals = C::Normals::Present;
    Check(!C::Exclude(body), "a rigid surface with normals stays eligible");

    body.normals = C::Normals::Absent;
    Check(C::Exclude(body) == C::Exclusion::NoNormals, "a rigid surface without normals is rejected");

    body.skinning = C::Skinning::Skinned;
    Check(!C::Exclude(body), "skinned geometry is exempt from the normal requirement");

    body.skinning = C::Skinning::Rigid;
    body.role     = C::Role::Decal;
    Check(C::Exclude(body) == C::Exclusion::Decal, "a decal keeps its more specific reason");
    body.role          = C::Role::Ordinary;
    body.materialAlpha = 0;
    Check(C::Exclude(body) == C::Exclusion::Invisible, "invisibility still outranks the vertex format");
  }

  void NativeFadeCoverage()
  {
    // Regression: a scaled positive cutoff can write invisible depth.
    const float oldReference = 128 * 0.5f;
    Check(80 >= oldReference + 1 && 80 * 0.5f < oldReference, "fixture exposes depth-writing texels rejected by colour");

    const float reference = C::silhouetteCutoutReference;
    Check(0 < reference + 1, "empty texture pixels do not write native depth");
    bool visibleDepth = true;
    bool blendRoute   = true;
    for (unsigned step = 1; step <= 256; ++step)
    {
      const float requested  = step / 256.0f;
      const float opacity    = C::ObjectFadeOpacity(requested);
      blendRoute            &= opacity > 0 && opacity < 1;
      for (unsigned texel = 0; texel <= 255; ++texel)
      {
        const bool writesDepth        = texel >= reference + 1;
        const bool contributesColour  = texel * opacity > 0 && texel * opacity >= reference;
        visibleDepth                 &= !writesDepth || contributesColour;
      }
    }
    Check(visibleDepth, "every depth-writing texel contributes colour throughout fade");
    Check(blendRoute, "all visible requested opacities retain native blended fade");
    Check(C::ObjectFadeOpacity(0) == 0, "zero opacity remains invisible");
    Check(C::ObjectFadeOpacity(0.5f) == 0.5f, "ordinary requested opacity is unchanged");
    Check(1 - C::ObjectFadeOpacity(1) < 0.0000001f, "full requested opacity differs by less than one ten-millionth");
  }

}

int main()
{
  SurfaceSelection();
  VertexFormatCoverage();
  NativeFadeCoverage();
  if (failures) return 1;
  std::puts("Surface selection and native fade coverage contracts passed");
  return 0;
}
