module;

#include "PCH.h"
#include "GhostTrailsAPI.h"

export module GhostTrails.Runtime;
import GhostTrails.Requests;
import GhostTrails.Timeline;
import GhostTrails.Geometry;

export namespace GhostTrails
{
    // This owner is called only from Main::Update. Actor references are resolved
    // into temporary NiPointers; pools own geometry, never an actor's lifetime.
    class Runtime
    {
    public:
        Runtime(Requests& requests, Limits limits) : requests_(requests), limits_(limits) {}

        void Reset()
        {
            for (auto& [handle, actor] : actors_) actor.pool.Clear();
            actors_.clear();
            work_.clear();
            gameTime_ = 0;
        }

        void Tick(float delta)
        {
            if (std::isfinite(delta) && delta > 0) gameTime_ += delta;
            const auto wallTime = std::chrono::steady_clock::now();
            auto snapshot = requests_.Snapshot();
            std::uint32_t cloneBudget = limits_.clonesPerFrame;

            for (auto& [handle, entry] : actors_) {
                if (entry.gone) continue;
                auto resolved = Resolve(handle);
                if (!resolved) {
                    entry.ClearSource();
                    entry.gone = true; // A failed handle never becomes valid again.
                    continue;
                }
                const auto& actor = *resolved;

                const auto* cell = actor->GetParentCell();
                auto* object = actor->Get3D(false);
                auto* root = object ? object->AsNode() : nullptr;
                const auto cellID = cell ? cell->GetFormID() : 0;
                if (root != entry.root.get() || cellID != entry.cell || (root && root->parent != entry.parent.get())) {
                    entry.pool.Clear();
                    entry.root = RE::NiPointer<RE::NiNode>{root};
                    entry.parent = root ? RE::NiPointer<RE::NiNode>{root->parent} : nullptr;
                    entry.cell = cellID;
                }
                entry.pool.Tick(gameTime_);
                entry.checked = false;
            }

            // Rotate admission order so a long Prepare cannot monopolize the
            // shared per-frame cold-clone budget.
            if (!snapshot.empty()) {
                cursor_ %= snapshot.size();
                std::rotate(snapshot.begin(), snapshot.begin() + cursor_, snapshot.end());
                ++cursor_;
            }
            for (const auto& request : snapshot) {
                if (request.canceled || request.released) {
                    for (auto& [handle, entry] : actors_) entry.pool.Cancel(request.id);
                    work_.erase(request.id);
                    requests_.AcknowledgeStop(request.id);
                    continue;
                }
                if (Terminal(request.status.state)) continue;

                auto [it, inserted] = work_.try_emplace(request.id, request.options, gameTime_);
                auto& timeline = it->second;
                const auto actor = actors_.find(request.actor);
                if (actor != actors_.end() && actor->second.gone) timeline.Fail(API::Failure::ActorGone);
                else if (timeline.Due(gameTime_)) Process(request, timeline, cloneBudget, wallTime);
                requests_.Publish(request.id, timeline.Status());
                if (timeline.Finished()) {
                    const auto result = timeline.Status();
                    logger::info("request={} actor={:08X} finished state={} created={} missed={} attempts={} lastFailure={}",
                        request.id, request.actor, static_cast<unsigned>(result.state), result.spawned,
                        result.missed, result.attempts, static_cast<unsigned>(result.lastFailure));
                    work_.erase(it);
                }
            }

            std::erase_if(actors_, [&](auto& item) {
                auto& entry = item.second;
                const bool requested = std::ranges::any_of(snapshot, [&](const auto& request) {
                    return request.actor == item.first && !Terminal(request.status.state) && !request.canceled;
                });
                if (requested) return false;
                const auto idle = std::chrono::duration<float>(wallTime - entry.lastUse).count();
                if (entry.gone || (!entry.pool.HasActive() && idle >= limits_.idleSeconds)) {
                    return true;
                }
                return false;
            });
        }

    private:
        struct ActorPool
        {
            Geometry::Pool pool;
            RE::NiPointer<RE::NiNode> root;
            RE::NiPointer<RE::NiNode> parent;
            std::chrono::steady_clock::time_point lastUse{std::chrono::steady_clock::now()};
            RE::FormID cell{};
            bool checked{};
            bool gone{};

            void ClearSource()
            {
                pool.Clear();
                root.reset();
                parent.reset();
                checked = false;
            }
        };

        static std::expected<RE::NiPointer<RE::Actor>, API::Failure> Resolve(API::ActorHandle handle)
        {
            RE::NiPointer<RE::TESObjectREFR> reference;
            if (!RE::LookupReferenceByHandle(handle, reference) || !reference)
                return std::unexpected(API::Failure::ActorGone);
            auto* actor = reference->As<RE::Actor>();
            if (!actor) return std::unexpected(API::Failure::ActorGone);
            return RE::NiPointer<RE::Actor>{actor};
        }

        static API::Failure Failure(Geometry::Error error)
        {
            switch (error) {
            case Geometry::Error::CloneFailed: return API::Failure::CloneFailed;
            case Geometry::Error::NoGeometry: return API::Failure::NoGeometry;
            case Geometry::Error::InvalidSkin: return API::Failure::InvalidSkin;
            case Geometry::Error::PoolBusy: return API::Failure::Capacity;
            case Geometry::Error::NoSource: return API::Failure::No3D;
            case Geometry::Error::SourceChanged: return API::Failure::SceneChanged;
            case Geometry::Error::NotInstalled: return API::Failure::CloneFailed;
            }
            return API::Failure::CloneFailed;
        }

        void Retry(const Request& request, Timeline& timeline, API::Failure failure)
        {
            const auto previous = timeline.Status();
            const bool waiting = timeline.Retry(failure, gameTime_);
            if (previous.lastFailure != failure || previous.attempts == 0 || !waiting) {
                logger::warn("request={} actor={:08X} emission={} failure={} attempt={} retry={}",
                    request.id, request.actor, previous.spawned + previous.missed + 1,
                    static_cast<unsigned>(failure), timeline.Status().attempts, waiting);
            }
        }

        void Process(const Request& request, Timeline& timeline, std::uint32_t& budget,
            std::chrono::steady_clock::time_point wallTime)
        {
            auto found = actors_.find(request.actor);
            if (found == actors_.end()) {
                if (actors_.size() >= limits_.actors) {
                    Retry(request, timeline, API::Failure::Capacity);
                    return;
                }
                found = actors_.try_emplace(request.actor).first;
            }
            auto& entry = found->second;
            entry.lastUse = wallTime;

            if (entry.gone) {
                timeline.Fail(API::Failure::ActorGone);
                return;
            }
            auto resolved = Resolve(request.actor);
            if (!resolved) {
                entry.gone = true;
                entry.ClearSource();
                timeline.Fail(API::Failure::ActorGone);
                logger::warn("request={} actor={:08X} permanently unresolved handle", request.id, request.actor);
                return;
            }
            const auto& actor = *resolved;

            auto* object = actor->Get3D(false);
            auto* root = object ? object->AsNode() : nullptr;
            if (!root || !root->parent) {
                Retry(request, timeline, API::Failure::No3D);
                return;
            }
            if (!entry.checked) {
                if (!entry.pool.Matches(root) && !entry.pool.Initialize(root)) {
                    Retry(request, timeline, API::Failure::No3D);
                    return;
                }
                entry.checked = true;
                entry.root = RE::NiPointer<RE::NiNode>{root};
                entry.parent = RE::NiPointer<RE::NiNode>{root->parent};
                const auto* cell = actor->GetParentCell();
                entry.cell = cell ? cell->GetFormID() : 0;
            }

            if (request.prepare) {
                // Existing reusable slots count toward the requested ready capacity.
                if (entry.pool.Size() >= request.options.count) {
                    while (!timeline.Finished()) timeline.Success(gameTime_);
                    return;
                }
            } else {
                const auto emitted = entry.pool.Spawn(gameTime_, request.options.lifetime,
                    request.options.appearance, request.id);
                if (emitted) {
                    timeline.Success(gameTime_);
                    return;
                }
                if (emitted.error() != Geometry::Error::PoolBusy) {
                    Retry(request, timeline, Failure(emitted.error()));
                    return;
                }
            }

            std::size_t totalSlots{};
            for (const auto& [handle, actorPool] : actors_) totalSlots += actorPool.pool.Size();
            if (entry.pool.Size() >= limits_.slotsPerActor || totalSlots >= limits_.totalSlots) {
                Retry(request, timeline, API::Failure::Capacity);
                return;
            }
            if (!budget) return;
            --budget;
            const auto grown = entry.pool.Grow();
            if (!grown) {
                Retry(request, timeline, Failure(grown.error()));
                return;
            }
            if (request.prepare) {
                if (entry.pool.Size() >= request.options.count)
                    while (!timeline.Finished()) timeline.Success(gameTime_);
                return;
            }

            const auto emitted = entry.pool.Spawn(gameTime_, request.options.lifetime,
                request.options.appearance, request.id);
            if (emitted) timeline.Success(gameTime_);
            else Retry(request, timeline, Failure(emitted.error()));
        }

        Requests& requests_;
        Limits limits_;
        std::unordered_map<API::ActorHandle, ActorPool> actors_;
        std::unordered_map<API::RequestID, Timeline> work_;
        double gameTime_{};
        std::size_t cursor_{};
    };
}
