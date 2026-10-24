module;

#include <cstdint>
#include <optional>
#include <string_view>

export module GhostTrails.Coverage;

export namespace GhostTrails::Coverage
{
    enum class Shading { Unsupported, Lighting, Effect };
    enum class Skinning { Rigid, Skinned };
    enum class Role { Ordinary, Decal, WeaponBlood };
    enum class Exclusion { Overlay, Invisible, Decal, AuxiliaryEffect, Unsupported };

    struct SourceSurface
    {
        std::string_view name;
        Shading shading;
        Skinning skinning;
        Role role;
        float propertyAlpha;
        float materialAlpha;
    };

    std::optional<Exclusion> Exclude(const SourceSurface& source)
    {
        if (source.name.find(" [Ovl") != source.name.npos || source.name.find(" [SOvl") != source.name.npos) {
            return Exclusion::Overlay;
        }
        if (source.propertyAlpha <= 0 || source.materialAlpha <= 0) return Exclusion::Invisible;
        if (source.shading == Shading::Unsupported) return Exclusion::Unsupported;
        if (source.role == Role::WeaponBlood) return Exclusion::Decal;
        if (source.skinning == Skinning::Rigid) {
            if (source.role == Role::Decal) return Exclusion::Decal;
            if (source.shading == Shading::Effect) return Exclusion::AuxiliaryEffect;
        }
        return std::nullopt;
    }

    // The lighting shader tests textureAlpha * materialAlpha against an 8-bit
    // reference. Scale that reference with the ghost fade so an opaque strand
    // does not disappear while the requested material opacity is still visible.
    class Cutout
    {
    public:
        explicit constexpr Cutout(std::uint8_t reference) : reference_(reference) {}

        // opacity is already validated by request admission / lifetime sampling.
        // Rounding down does not reject texels that passed the authored cutoff.
        constexpr std::uint8_t Reference(float opacity) const
        {
            return static_cast<std::uint8_t>(reference_ * opacity);
        }

    private:
        std::uint8_t reference_;
    };
}
