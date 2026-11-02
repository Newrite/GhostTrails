module;

#include <cstdint>
#include <optional>
#include <string_view>

export module GhostTrails.Coverage;

export namespace GhostTrails::Coverage
{

  enum class Shading
  {
    Unsupported,
    Lighting,
    Effect
  };
  enum class Skinning
  {
    Rigid,
    Skinned
  };
  enum class Role
  {
    Ordinary,
    Decal,
    WeaponBlood
  };
  // Vertex-format evidence. Unknown means the description was not populated when the
  // surface was inspected, which proves nothing and must not exclude it.
  enum class Normals
  {
    Unknown,
    Present,
    Absent
  };
  enum class Exclusion
  {
    Overlay,
    Invisible,
    NoNormals,
    Decal,
    AuxiliaryEffect,
    Unsupported
  };

  struct SourceSurface
  {
    std::string_view name;
    Shading          shading;
    Skinning         skinning;
    Role             role;
    float            propertyAlpha;
    float            materialAlpha;
    Normals          normals{Normals::Unknown};
  };

  std::optional<Exclusion> Exclude(const SourceSurface& source)
  {
    if (source.name.find(" [Ovl") != source.name.npos || source.name.find(" [SOvl") != source.name.npos)
    {
      return Exclusion::Overlay;
    }
    if (source.propertyAlpha <= 0 || source.materialAlpha <= 0) return Exclusion::Invisible;
    if (source.shading == Shading::Unsupported) return Exclusion::Unsupported;
    if (source.role == Role::WeaponBlood) return Exclusion::Decal;
    if (source.skinning == Skinning::Rigid)
    {
      if (source.role == Role::Decal) return Exclusion::Decal;
      if (source.shading == Shading::Effect) return Exclusion::AuxiliaryEffect;
    }
    // A rigid surface whose vertex format carries no normal renders black or plastic
    // under the fresh lighting material built for a ghost. Skinned geometry is exempt:
    // its authored layout stays in charge. Checked last so a more specific reason wins.
    if (source.skinning == Skinning::Rigid && source.normals == Normals::Absent)
    {
      return Exclusion::NoNormals;
    }
    return std::nullopt;
  }

  // Both passes share NiAlphaProperty, but only the colour shader multiplies
  // texture alpha by object fade. A positive shared cutoff would therefore
  // produce invisible depth-writing texels as the ghost fades. Zero keeps
  // soft coverage in colour; native Utility depth rejects alpha below 1/255.
  inline constexpr std::uint8_t silhouetteCutoutReference = 0;

  constexpr float ObjectFadeOpacity(float opacity)
  {
    // Admission guarantees [0,1]. Keep even requested opacity 1 in the
    // native fade route, which requires strictly less than one. Otherwise
    // blended hair switches to the opaque route for its first frame.
    constexpr float maximumBlendedFade = 0x1.fffffep-1f;
    return opacity == 1 ? maximumBlendedFade : opacity;
  }

}
