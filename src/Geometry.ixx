module;

#include "PCH.h"
#include "GhostTrailsAPI.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module GhostTrails.Geometry;
import GhostTrails.Materials;
import GhostTrails.Coverage;

namespace GhostTrails::Geometry
{
    namespace API = GhostTrailsAPI;
    using Flag = RE::NiAVObject::Flag;
    using ShaderFlag = RE::BSShaderProperty::EShaderPropertyFlag;
    using CloneMap = std::unordered_map<RE::NiAVObject*, RE::NiAVObject*>;

    namespace Address
    {
        constexpr auto ObjectNetCopy = REL::VariantID(69139, 70500, 0xCA5580);
        constexpr auto ObjectNetProcess = REL::VariantID(69162, 70523, 0xCA6A50);
    }

    // These are NiObjectNET's controller edges, not a replacement for cloning
    // geometry or skin. Every other engine cloning process retains its behavior.
    thread_local RE::NiCloningProcess* snapshotProcess{};
    bool installed{};

    using CopyFunction = RE::NiObject*(RE::NiObject*, RE::NiCloningProcess&);
    using ProcessFunction = void(RE::NiObject*, RE::NiCloningProcess&);
    REL::Relocation<CopyFunction> previousCopy;
    REL::Relocation<ProcessFunction> previousProcess;

    RE::NiObject* NativeCopyController(RE::NiObject* controller, RE::NiCloningProcess& process)
    {
        return controller->CreateClone(process);
    }

    void NativeProcessController(RE::NiObject* controller, RE::NiCloningProcess& process)
    {
        controller->ProcessClone(process);
    }

    RE::NiObject* CopyController(RE::NiObject* controller, RE::NiCloningProcess& process)
    {
        return snapshotProcess == &process ? nullptr : previousCopy(controller, process);
    }

    void ProcessController(RE::NiObject* controller, RE::NiCloningProcess& process)
    {
        if (snapshotProcess != &process) previousProcess(controller, process);
    }

    bool KnownRuntime()
    {
        const auto version = REL::Module::get().version();
        return (REL::Module::IsSE() && version == REL::Version{1, 5, 97, 0}) ||
               (REL::Module::IsAE() && version == REL::Version{1, 6, 1170, 0}) ||
               (REL::Module::IsVR() && version == REL::Version{1, 4, 15, 0});
    }

    // The three audited clone paths allocate an eight-byte array cookie, but
    // their destructors free boneEntries directly. Move the entries over that
    // cookie; this repair itself never allocates and cannot fail under pressure.
    void Normalize(RE::BSFlattenedBoneTree& tree)
    {
        auto& data = tree.GetRuntimeData();
        if (!data.boneEntries) return;

        auto* allocation = reinterpret_cast<std::byte*>(data.boneEntries) - sizeof(std::uint64_t);
        if (data.numBones == 0) {
            RE::free(allocation);
            data.boneEntries = nullptr;
            return;
        }

        const auto bytes = static_cast<std::size_t>(data.numBones) * sizeof(RE::BSFlattenedBoneTree::BoneEntry);
        std::memmove(allocation, data.boneEntries, bytes);
        data.boneEntries = reinterpret_cast<RE::BSFlattenedBoneTree::BoneEntry*>(allocation);
    }

    struct Clone
    {
        RE::NiPointer<RE::NiNode> root;
        CloneMap pairs;
    };

    Clone CopyTree(RE::NiNode& root)
    {
        RE::NiCloningProcess process{};
        process.copyType = 1;
        process.scale = {1, 1, 1};

        struct Scope
        {
            RE::NiCloningProcess* previous;
            ~Scope() { snapshotProcess = previous; }
        } scope{std::exchange(snapshotProcess, &process)};

        RE::NiPointer<RE::NiObject> holder{root.CreateClone(process)};
        if (holder) root.ProcessClone(process);

        // Normalize every independently cloned tree before any caller can
        // reject and destroy the clone. The identity map also includes skins,
        // properties and extra data; only scene-node pairs belong in the pool.
        Clone result;
        std::unordered_set<RE::BSFlattenedBoneTree*> normalized;
        for (const auto& [original, copied] : process.cloneMap) {
            if (original == copied) continue;
            auto* from = netimmerse_cast<RE::NiAVObject*>(original);
            auto* to = netimmerse_cast<RE::NiAVObject*>(copied);
            if (!from || !to) continue;

            if (auto* tree = netimmerse_cast<RE::BSFlattenedBoneTree*>(to); tree && normalized.insert(tree).second) {
                Normalize(*tree);
            }
            result.pairs.emplace(from, to);
        }
        if (holder) result.root.reset(holder->AsNode());
        return result;
    }

    bool Collect(RE::NiNode* root, std::vector<RE::NiAVObject*>& output)
    {
        output.clear();
        if (!root) return false;

        std::unordered_set<RE::NiAVObject*> visited;
        std::vector<RE::NiAVObject*> pending{root};
        while (!pending.empty()) {
            auto* object = pending.back();
            pending.pop_back();
            if (!visited.insert(object).second) return false;
            output.push_back(object);

            if (auto* node = object->AsNode()) {
                const auto& children = node->GetChildren();
                for (std::size_t i = children.capacity(); i > 0; --i) {
                    const auto& child = children[static_cast<std::uint16_t>(i - 1)];
                    if (child) pending.push_back(child.get());
                }
            }
        }
        return true;
    }

    const char* Name(const RE::NiAVObject& object)
    {
        return object.name.c_str() ? object.name.c_str() : "<unnamed>";
    }

    struct Stamp
    {
        RE::NiAVObject* object{};
        RE::NiNode* parent{};
        RE::NiSkinInstance* skin{};
        RE::NiSkinPartition* partition{};
        RE::BSShaderProperty* shader{};
        RE::BSShaderMaterial* material{};
        const RE::BSFlattenedBoneTree::BoneEntry* bones{};
        std::uint32_t boneCount{};
        std::uint16_t vertices{}, triangles{};
        const void* dynamicData{};
        std::uint32_t dynamicBytes{};

        bool operator==(const Stamp&) const = default;
    };

    Stamp Inspect(RE::NiAVObject& object, bool root)
    {
        Stamp stamp{&object, root ? nullptr : object.parent};
        if (auto* geometry = object.AsGeometry()) {
            const auto& data = geometry->GetGeometryRuntimeData();
            stamp.skin = data.skinInstance.get();
            stamp.partition = stamp.skin ? stamp.skin->skinPartition.get() : nullptr;
            stamp.shader = data.shaderProperty.get();
            stamp.material = stamp.shader ? stamp.shader->material : nullptr;
        }
        if (auto* tree = netimmerse_cast<RE::BSFlattenedBoneTree*>(&object)) {
            const auto& data = tree->GetRuntimeData();
            stamp.bones = data.boneEntries;
            stamp.boneCount = data.numBones;
        }
        if (auto* tri = object.AsTriShape()) {
            const auto& data = tri->GetTrishapeRuntimeData();
            stamp.vertices = data.vertexCount;
            stamp.triangles = data.triangleCount;
        }
        if (auto* dynamic = object.AsDynamicTriShape()) {
            auto& data = dynamic->GetDynamicTrishapeRuntimeData();
            RE::BSSpinLockGuard guard(data.lock);
            stamp.dynamicData = data.dynamicData;
            stamp.dynamicBytes = data.dataSize;
        }
        return stamp;
    }

    const char* Exclusion(RE::BSGeometry& geometry)
    {
        const auto& data = geometry.GetGeometryRuntimeData();
        Coverage::SourceSurface source{Name(geometry), Coverage::Shading::Unsupported,
            data.skinInstance ? Coverage::Skinning::Skinned : Coverage::Skinning::Rigid,
            Coverage::Role::Ordinary, 1, 1};

        if (const auto* lit = netimmerse_cast<RE::BSLightingShaderProperty*>(data.shaderProperty.get()); lit && lit->material) {
            source.shading = Coverage::Shading::Lighting;
            source.propertyAlpha = lit->alpha;
            source.materialAlpha = static_cast<RE::BSLightingShaderMaterialBase*>(lit->material)->materialAlpha;
            if (lit->flags.all(ShaderFlag::kWeaponBlood)) source.role = Coverage::Role::WeaponBlood;
            else if (lit->flags.all(ShaderFlag::kDecal)) source.role = Coverage::Role::Decal;
        } else if (auto* effect = netimmerse_cast<RE::BSEffectShaderProperty*>(data.shaderProperty.get()); effect && effect->GetMaterial()) {
            source.shading = Coverage::Shading::Effect;
            source.propertyAlpha = effect->alpha;
            source.materialAlpha = effect->GetMaterial()->baseColor.alpha;
        }

        const auto excluded = Coverage::Exclude(source);
        if (!excluded) return nullptr;
        switch (*excluded) {
        case Coverage::Exclusion::Overlay: return "RaceMenu overlay";
        case Coverage::Exclusion::Invisible: return "invisible source surface";
        case Coverage::Exclusion::Decal: return "blood/rigid decal";
        case Coverage::Exclusion::AuxiliaryEffect: return "unskinned auxiliary effect";
        case Coverage::Exclusion::Unsupported: return "unsupported lighting/effect material";
        }
        return "unsupported lighting/effect material";
    }

    using TransformMap = std::unordered_map<const RE::NiTransform*, const RE::NiTransform*>;

    bool BindTransforms(const CloneMap& pairs, TransformMap& transforms)
    {
        for (const auto& [source, target] : pairs) {
            transforms.emplace(&source->local, &target->local);
            transforms.emplace(&source->world, &target->world);
            auto* sourceTree = netimmerse_cast<RE::BSFlattenedBoneTree*>(source);
            if (!sourceTree) continue;
            auto* targetTree = netimmerse_cast<RE::BSFlattenedBoneTree*>(target);
            if (!targetTree) return false;

            const auto& original = sourceTree->GetRuntimeData();
            const auto& copy = targetTree->GetRuntimeData();
            if (original.numBones != copy.numBones || (original.numBones && (!original.boneEntries || !copy.boneEntries))) {
                return false;
            }
            for (std::uint32_t i = 0; i < original.numBones; ++i) {
                transforms.emplace(&original.boneEntries[i].local, &copy.boneEntries[i].local);
                transforms.emplace(&original.boneEntries[i].world, &copy.boneEntries[i].world);
            }
        }
        return true;
    }

    bool BindSkin(RE::BSGeometry& source, RE::BSGeometry& target, const CloneMap& pairs,
                  const TransformMap& transforms, RE::NiNode& root)
    {
        const auto* original = source.GetGeometryRuntimeData().skinInstance.get();
        auto* copy = target.GetGeometryRuntimeData().skinInstance.get();
        if (!original) return !copy;
        if (!copy || original == copy || !original->skinData || !copy->skinData) return false;

        const auto count = original->skinData->GetBoneCount();
        if (copy->skinData->GetBoneCount() != count) return false;
        if (count && (!copy->bones || !copy->boneWorldTransforms ||
                      original->bones == copy->bones || original->boneWorldTransforms == copy->boneWorldTransforms)) {
            return false;
        }

        for (std::uint32_t i = 0; i < count; ++i) {
            const auto* transform = original->boneWorldTransforms ? original->boneWorldTransforms[i] : nullptr;
            auto* bone = original->bones ? original->bones[i] : nullptr;
            if (!transform && bone) transform = &bone->world;
            const auto found = transforms.find(transform);
            if (found == transforms.end()) return false;
            copy->boneWorldTransforms[i] = found->second;
            const auto targetBone = pairs.find(bone);
            copy->bones[i] = targetBone == pairs.end() ? nullptr : targetBone->second;
        }
        const auto parent = pairs.find(original->rootParent);
        copy->rootParent = parent == pairs.end() ? &root : parent->second;
        copy->frameID = (std::numeric_limits<std::uint32_t>::max)();
        return true;
    }

    struct Binding
    {
        RE::NiAVObject* source;
        RE::NiAVObject* target;
        bool hidden{};
        bool ignoreSourceHidden{};
    };

    struct TreeBinding
    {
        RE::BSFlattenedBoneTree* source;
        RE::BSFlattenedBoneTree* target;
        const RE::BSFlattenedBoneTree::BoneEntry* sourceEntries;
        std::uint32_t count;
    };

    struct Ghost
    {
        // Declared first so the graph and all shader borrows die before it.
        RE::NiPointer<RE::BSFadeNode> fade;
        RE::NiPointer<RE::NiNode> root;
        RE::NiPointer<RE::NiNode> parent;
        // Bindings borrow the pool's retained source nodes and this owned root.
        std::vector<Binding> bindings;
        std::vector<TreeBinding> trees;
        std::vector<Materials::Surface> surfaces;
        std::vector<RE::NiSkinInstance*> skins;

        struct Active
        {
            double born;
            float lifetime;
            API::Appearance look;
            std::uint32_t request;
#ifdef GHOSTTRAILS_DIAGNOSTICS
            bool passesLogged{};
#endif
        };
        std::optional<Active> active;

        Ghost() = default;
        Ghost(const Ghost&) = delete;
        Ghost& operator=(const Ghost&) = delete;
        Ghost(Ghost&&) = default;
        Ghost& operator=(Ghost&&) = delete;
        ~Ghost() { Detach(); }

        void Detach()
        {
            if (root) {
                root->GetFlags().set(Flag::kHidden);
                if (parent && root->parent == parent.get()) parent->DetachChild(root.get());
            }
            parent.reset();
            active.reset();
        }

        void ApplyLook(const API::Appearance& look, float visibility)
        {
            const float opacity = Coverage::ObjectFadeOpacity(look.alpha * visibility);
            auto& fadeState = fade->GetRuntimeData();
            fadeState.currentFade = opacity;
            // Alternate channel is multiplied only for selected LOD passes;
            // keep it neutral to avoid applying the same opacity twice.
            fadeState.unk14C = 1;
            for (auto& surface : surfaces) {
                surface.ApplyColor({look.red, look.green, look.blue});
                surface.property->emissiveMult = look.intensity;
                // Invalidate cached render passes when the requested shader
                // variant changes between uses of an inactive pooled ghost.
                const bool rim = look.rimPower > 0;
                if (surface.property->flags.all(ShaderFlag::kRimLighting) != rim) {
                    surface.property->SetFlags(RE::BSShaderProperty::EShaderPropertyFlag8::kRimLighting, rim);
                }
                surface.material->rimLightPower = look.rimPower;
            }
        }

#ifdef GHOSTTRAILS_DIAGNOSTICS
        void LogRenderedPasses()
        {
            // Called once on the next update after attachment. Empty lists can
            // mean the ghost was culled; this is evidence, not a render test.
            for (const auto& surface : surfaces) {
                std::uint32_t depth = 0, colour = 0, other = 0;
                const auto* first = surface.property->renderPassList.head;
                for (auto* pass = first; pass; pass = pass->next) {
                    if (pass->accumulationHint == 14) ++depth;
                    else if (pass->accumulationHint == 9) ++colour;
                    else ++other;
                }
                logger::debug("request={} native fade passes '{}': depth14={} colour9={} other={} ownedFade={} opacity={}",
                    active->request, first && first->geometry ? Name(*first->geometry) : "<not accumulated>",
                    depth, colour, other, surface.property->fadeNode == fade.get(), surface.property->alpha);
            }
        }
#endif
    };
}

export namespace GhostTrails::Geometry
{
    enum class Error
    {
        CloneFailed,
        NoGeometry,
        InvalidSkin,
        PoolBusy,
        NoSource,
        SourceChanged,
        NotInstalled
    };

    bool Install()
    {
        if (installed) return true;
        if (!KnownRuntime()) {
            logger::error("Ghost geometry unavailable: runtime {} has not been audited", REL::Module::get().version().string());
            return false;
        }

        const auto copy = Address::ObjectNetCopy.address() + REL::Relocate(0x259, 0x25B, 0x259);
        const auto process = Address::ObjectNetProcess.address() + 0x64;
        auto& trampoline = SKSE::GetTrampoline();
        const auto install = [&](std::uintptr_t site, auto hook, auto native, auto& previous) {
            previous = reinterpret_cast<std::uintptr_t>(native);
            const auto* instruction = reinterpret_cast<const std::uint8_t*>(site);
            if (instruction[0] == 0xE8) {
                previous = trampoline.write_call<5>(site, hook);
            } else {
                const bool indirect = instruction[0] == 0xFF && instruction[1] == 0x15;
                const auto slot = trampoline.write_call<6>(site, hook, true);
                // CommonLib returns the RIP pointer slot for an installed FF15.
                // Native FF90 dispatches virtually and has no fixed callee.
                if (indirect) previous = *reinterpret_cast<const std::uintptr_t*>(slot);
            }
        };

        install(copy, CopyController, NativeCopyController, previousCopy);
        install(process, ProcessController, NativeProcessController, previousProcess);
        installed = true;
        logger::info("Ghost controller hooks installed at {:X}/{:X}", copy, process);
        return true;
    }

    // Game-thread owner. Initialize/Matches/Grow/Spawn are demand-driven; Tick
    // reads only cloned state. No actor is retained and no scene scan occurs in
    // the idle path. The caller resolves its actor handle before using a pool.
    class Pool
    {
    public:
        Pool() = default;
        Pool(const Pool&) = delete;
        Pool& operator=(const Pool&) = delete;
        Pool(Pool&&) = default;
        Pool& operator=(Pool&&) = default;
        ~Pool() { Clear(); }

        void Clear()
        {
            ghosts_.clear();
            topology_.clear();
            sources_.clear();
            sourceRoot_.reset();
            white_.reset();
        }

        bool Initialize(RE::NiNode* root)
        {
            Clear();
            std::vector<RE::NiAVObject*> nodes;
            if (!Collect(root, nodes)) {
                logger::warn("Ghost source rejected: missing root, cycle or shared scene child");
                return false;
            }

            sourceRoot_.reset(root);
            sources_.reserve(nodes.size());
            topology_.reserve(nodes.size());
            for (auto* node : nodes) {
                sources_.emplace_back(node);
                topology_.push_back(Inspect(*node, node == root));
            }
            return true;
        }

        bool Matches(RE::NiNode* root)
        {
            if (!root || root != sourceRoot_.get()) return false;
            std::vector<RE::NiAVObject*> nodes;
            if (!Collect(root, nodes) || nodes.size() != topology_.size()) return false;
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                if (Inspect(*nodes[i], nodes[i] == root) != topology_[i]) return false;
            }
            return true;
        }

        std::expected<void, Error> Grow()
        {
            if (!installed) return std::unexpected(Error::NotInstalled);
            if (!sourceRoot_) return std::unexpected(Error::NoSource);
#ifdef GHOSTTRAILS_DIAGNOSTICS
            const auto started = std::chrono::steady_clock::now();
#endif

            auto clone = CopyTree(*sourceRoot_);
            if (!clone.root) {
                logger::warn("Ghost clone failed: native CreateClone returned no node");
                return std::unexpected(Error::CloneFailed);
            }

            std::vector<RE::NiAVObject*> copied;
            if (!Collect(clone.root.get(), copied)) {
                logger::warn("Ghost clone rejected: invalid cloned scene topology");
                return std::unexpected(Error::CloneFailed);
            }
            std::unordered_set<RE::NiAVObject*> sourceNodes;
            for (const auto& node : sources_) sourceNodes.insert(node.get());
            for (auto* node : copied) {
                if (sourceNodes.contains(node)) {
                    logger::warn("Ghost clone rejected: native clone retained live scene node '{}'", Name(*node));
                    return std::unexpected(Error::CloneFailed);
                }
            }

            const std::unordered_set<RE::NiAVObject*> copiedNodes(copied.begin(), copied.end());
            std::erase_if(clone.pairs, [&](const auto& pair) {
                return !sourceNodes.contains(pair.first) || !copiedNodes.contains(pair.second);
            });
            TransformMap transforms;
            if (!BindTransforms(clone.pairs, transforms)) {
                logger::warn("Ghost clone rejected: flattened skeleton does not match its source");
                return std::unexpected(Error::InvalidSkin);
            }

            if (!white_) {
                RE::NiPointer<RE::NiTexture> texture;
                RE::BSShaderManager::GetTexture("textures\\effects\\fxwhite.dds", true, texture, false);
                white_.reset(netimmerse_cast<RE::NiSourceTexture*>(texture.get()));
                if (!white_) logger::warn("Ghost white texture unavailable: source diffuse textures retained");
            }

            auto fade = Materials::CreateFadeControl();
            if (!fade) {
                logger::warn("Ghost clone failed: {}", Materials::Describe(fade.error()));
                return std::unexpected(Error::CloneFailed);
            }

            Ghost ghost;
            ghost.fade = std::move(*fade);
            ghost.root = std::move(clone.root);
            ghost.root->GetFlags().set(Flag::kHidden);
            std::unordered_map<RE::NiAVObject*, RE::NiAVObject*> originals;
            for (const auto& [source, target] : clone.pairs) originals.emplace(target, source);

            std::size_t omitted = 0;
            for (auto* target : copied) {
                target->SetUserData(nullptr);
                target->collisionObject.reset();
                target->controllers.reset();
                target->RemoveAllExtraData();

                const auto found = originals.find(target);
                if (found == originals.end()) {
                    target->GetFlags().set(Flag::kHidden);
                    ++omitted;
                    continue;
                }

                auto* source = found->second;
                Binding binding{source, target, !source->AsNode() && !source->AsGeometry(),
                                source == sourceRoot_.get() || netimmerse_cast<RE::BSFaceGenNiNode*>(source) != nullptr};
                if (binding.hidden) {
                    target->GetFlags().set(Flag::kHidden);
                    ++omitted;
                }
                if (auto* tree = netimmerse_cast<RE::BSFlattenedBoneTree*>(source)) {
                    const auto& data = tree->GetRuntimeData();
                    ghost.trees.push_back({tree, static_cast<RE::BSFlattenedBoneTree*>(target), data.boneEntries, data.numBones});
                }

                if (auto* geometry = target->AsGeometry()) {
                    auto* original = source->AsGeometry();
                    const char* exclusion = original ? Exclusion(*original) : "cloned geometry has no source geometry";
                    if (exclusion) {
                        binding.hidden = true;
#if defined(GHOSTTRAILS_DIAGNOSTICS)
                        logger::debug("Ghost surface '{}' excluded: {}", Name(*target), exclusion);
#endif
                    } else if (!BindSkin(*original, *geometry, clone.pairs, transforms, *ghost.root)) {
                        binding.hidden = true;
                        logger::warn("Ghost surface '{}' omitted: skin is shared or references a bone outside the source graph", Name(*target));
                    } else if (auto surface = Materials::MakeSurface(*original, *geometry, white_, *ghost.fade)) {
                        ghost.surfaces.push_back(*surface);
                        if (auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get()) ghost.skins.push_back(skin);
                    } else {
                        binding.hidden = true;
                        logger::warn("Ghost surface '{}' omitted: {}", Name(*target), Materials::Describe(surface.error()));
                    }
                    if (binding.hidden) {
                        target->GetFlags().set(Flag::kHidden);
                        ++omitted;
                    }
                }
                ghost.bindings.push_back(binding);
            }

            if (ghost.surfaces.empty()) {
                logger::warn("Ghost clone failed: no usable geometry ({} omitted objects)", omitted);
                return std::unexpected(Error::NoGeometry);
            }
            if (omitted) logger::warn("Ghost clone usable with {} surfaces; {} auxiliary/unsupported objects omitted", ghost.surfaces.size(), omitted);
            ghosts_.push_back(std::move(ghost));
#ifdef GHOSTTRAILS_DIAGNOSTICS
            logger::debug("Cold clone complete: slots={} nodes={} surfaces={} durationMs={:.3f}",
                ghosts_.size(), copied.size(), ghosts_.back().surfaces.size(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
#endif
            return {};
        }

        std::expected<void, Error> Spawn(double now, float lifetime, const API::Appearance& look, std::uint32_t requestID)
        {
#ifdef GHOSTTRAILS_DIAGNOSTICS
            const auto started = std::chrono::steady_clock::now();
#endif
            if (!sourceRoot_ || !sourceRoot_->parent) return std::unexpected(Error::NoSource);
            const auto found = std::find_if(ghosts_.begin(), ghosts_.end(), [](const Ghost& ghost) { return !ghost.active; });
            if (found == ghosts_.end()) return std::unexpected(Error::PoolBusy);
            auto& ghost = *found;

            for (const auto& tree : ghost.trees) {
                const auto& source = tree.source->GetRuntimeData();
                const auto& target = tree.target->GetRuntimeData();
                if (source.boneEntries != tree.sourceEntries || source.numBones != tree.count || target.numBones != tree.count) {
                    return std::unexpected(Error::SourceChanged);
                }
            }
            for (const auto& binding : ghost.bindings) {
                auto& source = *binding.source;
                auto& target = *binding.target;
                target.local = source.local;
                target.world = source.world;
                target.previousWorld = source.world;
                target.worldBound = source.worldBound;
                const auto hidden = binding.hidden || (!binding.ignoreSourceHidden && source.GetFlags().all(Flag::kHidden));
                target.GetFlags().set(hidden, Flag::kHidden);

                if (auto* dynamic = source.AsDynamicTriShape(); dynamic && !binding.hidden) {
                    auto& sourceData = dynamic->GetDynamicTrishapeRuntimeData();
                    auto* targetDynamic = target.AsDynamicTriShape();
                    if (!targetDynamic) return std::unexpected(Error::SourceChanged);
                    auto& targetData = targetDynamic->GetDynamicTrishapeRuntimeData();
                    RE::BSSpinLockGuard guard(sourceData.lock);
                    if (sourceData.dataSize != targetData.dataSize ||
                        (sourceData.dataSize && (!sourceData.dynamicData || !targetData.dynamicData || sourceData.dynamicData == targetData.dynamicData))) {
                        return std::unexpected(Error::SourceChanged);
                    }
                    if (sourceData.dataSize) std::memcpy(targetData.dynamicData, sourceData.dynamicData, sourceData.dataSize);
                    targetData.frameCount = (std::numeric_limits<std::uint32_t>::max)();
                }
            }
            for (const auto& tree : ghost.trees) {
                const auto& source = tree.source->GetRuntimeData();
                auto& target = tree.target->GetRuntimeData();
                for (std::uint32_t i = 0; i < tree.count; ++i) {
                    target.boneEntries[i].local = source.boneEntries[i].local;
                    target.boneEntries[i].world = source.boneEntries[i].world;
                }
            }
            for (auto* skin : ghost.skins) skin->frameID = (std::numeric_limits<std::uint32_t>::max)();

            ghost.ApplyLook(look, 1);
            ghost.active = Ghost::Active{now, lifetime, look, requestID};
            ghost.parent.reset(sourceRoot_->parent);
            ghost.parent->AttachChild(ghost.root.get(), true);
            if (ghost.root->parent != ghost.parent.get()) {
                logger::warn("Ghost spawn failed: scene parent did not accept the detached clone");
                ghost.Detach();
                return std::unexpected(Error::NoSource);
            }
            ghost.root->GetFlags().reset(Flag::kHidden);
#ifdef GHOSTTRAILS_DIAGNOSTICS
            logger::debug("request={} pose attached: nodes={} surfaces={} lifetime={} durationMs={:.3f}",
                requestID, ghost.bindings.size(), ghost.surfaces.size(), lifetime,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
#endif
            return {};
        }

        void Tick(double now)
        {
            for (auto& ghost : ghosts_) {
                if (!ghost.active) continue;
#ifdef GHOSTTRAILS_DIAGNOSTICS
                if (!ghost.active->passesLogged) {
                    ghost.LogRenderedPasses();
                    ghost.active->passesLogged = true;
                }
#endif
                const auto elapsed = static_cast<float>(now - ghost.active->born);
                if (elapsed >= ghost.active->lifetime) {
                    ghost.Detach();
                } else {
                    ghost.ApplyLook(ghost.active->look, 1 - elapsed / ghost.active->lifetime);
                }
            }
        }

        void Cancel(std::uint32_t requestID)
        {
            for (auto& ghost : ghosts_) {
                if (ghost.active && ghost.active->request == requestID) ghost.Detach();
            }
        }

        bool HasActive() const
        {
            return std::any_of(ghosts_.begin(), ghosts_.end(), [](const Ghost& ghost) { return ghost.active.has_value(); });
        }

        std::size_t Size() const { return ghosts_.size(); }

    private:
        RE::NiPointer<RE::NiNode> sourceRoot_;
        std::vector<RE::NiPointer<RE::NiAVObject>> sources_;
        std::vector<Stamp> topology_;
        RE::NiPointer<RE::NiSourceTexture> white_;
        std::vector<Ghost> ghosts_;
    };
}
