# Dodge integration

`S:/Programming/Dodge/src/GhostTrail.cpp` is now an optional consumer of
`GhostTrails.dll` API v1. `GhostTrail::Install`, `OnPlayerDodgeStart` and `Reset`
retain their existing call sites. The consumer's player-update hook submits API
requests; it no longer creates, skins, attaches, fades or destroys scene objects.

Dodge still owns enable flags, dodge-kind selection, first-person policy and its
look/count/interval/lifetime/spawn-window settings. The standalone provider owns
snapshot creation, reusable geometry, retries, source invalidation and cleanup.
The old `GhostTrailTexture` setting is no longer consumed: the provider selects
its silhouette texture. Existing configuration/UI keys remain readable so this
focused migration does not rewrite unrelated Dodge settings code.

Intent from dodge/lifecycle callbacks is coalesced atomically. Settings and the
consumer's request list are accessed from player update. Requests receive an
actor handle, copied scalar options and a finite emission window. At most 16
request IDs are retained by this consumer; extreme input replaces the oldest
owned request. Terminal IDs are released after an additional full phantom
lifetime because `Release` also removes still-visible silhouettes. The provider
independently limits admission, actors and geometry slots.

Successive dodge starts are independent finite requests. Their windows may
overlap; the former embedded implementation restarted one shared emitter while
retaining its old visible ghosts. The API's `Cancel` removes visible ghosts as
well, so the consumer leaves accepted sequences to finish naturally. This
bounded overlap is a deliberate compatibility difference. Under pool pressure,
the provider may report missed emissions instead of overwriting a live ghost.

Absent or incompatible `GhostTrails.dll` only skips silhouettes and logs one
warning; dodging remains functional. Repeated identical admission failures are
logged once until successful admission or a different rejection. The standalone
plugin logs capture/geometry failures and the final request outcome.

The copied `include/GhostTrailsAPI.h` is the exact public header from this
project. Refresh that copy when changing an unpublished interface; published
interfaces must retain their ABI.

The migration does not demonstrate that the rest of Dodge supports AE/VR. Its
consumer uses only `PlayerCharacter::Update` slot `0xAD`, which is shared by the
three runtimes, and delegates the runtime-specific rendering to GhostTrails.

Verification: the existing `build/release-se` CMake configuration compiled the
changed consumer and linked the full `DodgeAll` target successfully against the
current local CommonLibSSE-NG source. A subsequent incremental build recompiled
the final consumer and linked successfully. Automatic game/mod output variables
were cleared in the build process; the DLL remains in the local build folder.
No game loading, visual behavior, or AE/VR build of the rest of Dodge was tested.
