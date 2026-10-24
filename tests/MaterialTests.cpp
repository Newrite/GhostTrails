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
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", contract);
            ++failures;
        }
    }

    void SurfaceSelection()
    {
        C::SourceSurface hair{"_BDO__PPW10A_Ba_45", C::Shading::Lighting,
            C::Skinning::Skinned, C::Role::Decal, 1, 1};
        Check(!C::Exclude(hair), "skinned Dint hair remains eligible despite its decal flag");
        hair.role = C::Role::WeaponBlood;
        Check(C::Exclude(hair) == C::Exclusion::Decal, "dedicated blood is still auxiliary when skinned");
        hair.role = C::Role::Decal;
        hair.skinning = C::Skinning::Rigid;
        Check(C::Exclude(hair) == C::Exclusion::Decal, "rigid decals are excluded");

        C::SourceSurface effect{"weapon fire", C::Shading::Effect,
            C::Skinning::Rigid, C::Role::Ordinary, 1, 1};
        Check(C::Exclude(effect) == C::Exclusion::AuxiliaryEffect, "rigid fire/effect geometry is not converted into actor clothing");
        effect.skinning = C::Skinning::Skinned;
        Check(!C::Exclude(effect), "skinned effect-shaded clothing remains eligible");
        effect.propertyAlpha = 0;
        Check(C::Exclude(effect) == C::Exclusion::Invisible, "invisible source properties are excluded");
        effect.propertyAlpha = 1;
        effect.materialAlpha = 0;
        Check(C::Exclude(effect) == C::Exclusion::Invisible, "invisible collider materials are excluded");
        effect.materialAlpha = 1;
        effect.shading = C::Shading::Unsupported;
        Check(C::Exclude(effect) == C::Exclusion::Unsupported, "unsupported materials are diagnosed");
        effect.name = "Body [SOvl0]";
        Check(C::Exclude(effect) == C::Exclusion::Overlay, "overlay exclusion is independent of material support");
    }

    void CutoutFade()
    {
        const C::Cutout hair{128};
        constexpr float quarterOpacity = 0.25f;
        // The old fixed cutoff rejects even an opaque strand at this fade.
        Check(255 * quarterOpacity < 128, "regression fixture actually exposes the early disappearance");
        Check(255 * quarterOpacity >= hair.Reference(quarterOpacity), "an opaque strand survives at the requested quarter opacity");
        Check(0 * quarterOpacity < hair.Reference(quarterOpacity), "empty texture pixels still fail the cutout");

        bool retained = true, bounded = true, fullOpacityUnchanged = true;
        for (unsigned reference = 0; reference < 256; ++reference) {
            const C::Cutout cutout{static_cast<std::uint8_t>(reference)};
            fullOpacityUnchanged &= cutout.Reference(1) == reference;
            for (unsigned step = 0; step <= 64; ++step) {
                const float opacity = step / 64.0f;
                const float threshold = cutout.Reference(opacity);
                const float authoredThreshold = reference * opacity;
                bounded &= threshold <= authoredThreshold && authoredThreshold - threshold < 1;
                for (unsigned texel = reference; texel < 256; ++texel) {
                    retained &= texel * opacity >= threshold;
                }
            }
        }
        Check(retained, "every originally accepted texel remains accepted throughout fade");
        Check(bounded, "fade reference quantization stays within one native byte step");
        Check(fullOpacityUnchanged, "full-opacity cutout preserves every authored reference");
    }
}

int main()
{
    SurfaceSelection();
    CutoutFade();
    if (failures) return 1;
    std::puts("Surface selection and fade-independent cutout coverage passed");
    return 0;
}
