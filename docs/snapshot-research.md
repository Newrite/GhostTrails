# Snapshot implementation evidence

The source review covered `Dodge/src/GhostTrail.cpp`, Dreamsleeve's
`src/Dreamsleeve.Client/Hooks.ixx` and `Game/Phantom/{Native,Capture,Scene}.ixx`,
and the local CommonLibSSE-NG headers and implementations. The binary checks
used the already open IDA databases for SE 1.5.97, AE 1.6.1170 and VR 1.4.15.
These are static observations, not in-game validation or timing measurements.

## Runtime addresses

| Operation | SE Address Library ID | AE ID | VR module-relative offset |
|---|---:|---:|---:|
| NiObjectNET CopyMembers | 69139 | 70500 | `0xCA5580` |
| NiObjectNET ProcessClone | 69162 | 70523 | `0xCA6A50` |
| NiAlphaProperty no-argument factory | 69311 | 70684 | `0xCADF10` |
| BSLightingShaderProperty no-argument factory | 99847 | 106492 | `0x1302ED0` |
| NiSourceTexture named factory | 69335 | 70717 | `0xCAEF60` |

`E:/Reverse/offsets-1.5.97.0.txt` contains relative offsets; the 1.6.1170 text
file contains full IDA addresses. They must not be interpreted interchangeably.
CommonLib's `VariantID` resolves SE and AE through their databases and takes a
raw module-relative offset for VR. Runtime-dependent members use accessors;
the stable `NiAVObject` local/world transforms precede the VR-dependent tail.

The two controller call instructions are six-byte register-indirect calls:

| Site | SE | AE | VR | Original bytes |
|---|---|---|---|---|
| CopyMembers controller CreateClone | `0x140C60229` | `0x140D25C3B` | `0x140CA57D9` | `FF 90 B8 00 00 00` |
| ProcessClone controller ProcessClone | `0x140C61504` | `0x140D26F24` | `0x140CA6AB4` | `FF 90 E8 00 00 00` |

The scoped thread-local cloning-process marker suppresses those controller
edges only for this plugin's own clone. Other callers pass through the saved
predecessor. Installation uses CommonLib's `write_call<5/6>` and retains its
returned address in a typed `REL::Relocation`. The native register-indirect
instruction uses the original virtual dispatch as its fallback. In this
CommonLib checkout, `write_call<6>` returns the RIP pointer slot for an installed
FF15, so that slot is dereferenced once to retain the actual handler. A direct
E8 uses the address returned by `write_call<5>` directly. There is no custom
instruction decoder, shared SDK module or strict original-bytes gate.

Dreamsleeve uses the same local installation pattern without a dependency on
GhostTrails. Both retain their own thread-local snapshot markers. Older
Dreamsleeve DLLs with the native-bytes-only guard still need rebuilding.
Live coexistence is still unverified.

## Cloned flattened-skeleton destruction

IDA showed the same allocator mismatch on all three runtimes:

| Runtime | CopyMembers | Destructor body |
|---|---|---|
| SE | `0x140C69220` | `0x140C69A60` |
| AE | `0x140D2F630` | `0x140D30BF0` |
| VR | `0x140CAF460` | `0x140CAFCA0` |

The clone allocates `boneCount * 0x80 + 8`, stores the count cookie, and retains
`allocation + 8` as boneEntries. The destructor releases every bone name and
frees boneEntries directly. A cloned tree must therefore be normalized before
any error branch can destroy it. The plugin moves the entries eight bytes down
over the cookie and retains the allocation base. No new allocation is required.
For a zero-length cloned array it frees the cookie allocation and clears the
pointer. The BoneEntry data contain transforms, indices, a node pointer and a
BSFixedString; moving the bytes transfers those resources without altering
their reference counts. The native clone identity map prevents name-based
pairing of duplicate skeleton/attachment names.

## Pool reuse and ownership

Dodge already pools complete skeleton clones and copies transforms at spawn.
Its unsafe escape hatch returns a live source transform for an unmapped skin
bone. The new renderer has no such fallback. Every rendered skin transform
must resolve into an owned cloned node or cloned flattened-bone entry. A bad
skin hides that surface with a warning; other surfaces remain usable. An actor
with no usable geometry produces a typed failure that the request owner may
retry. Material and alpha factory failures are likewise isolated per surface.

The renderer retains source scene objects with NiPointer only while the pool
exists. The caller retains an ActorHandle and must drop the pool after failed
resolution, replaced actor 3D, changed context or eviction. An expired handle
is never reconstructed from the FormID. Geometry stores no raw Actor pointer.
Node, material and skin pointers in bindings are explicit borrows from retained
source nodes and the ghost's owned root. Skin pointers never borrow live bone
transforms across that boundary.

Pool Initialize performs no cloning. Grow creates one detached clone. Matches
checks ordered scene identities, parent links, skins, partitions, shader and
material/diffuse/SRV/alpha-property identities and settings, flattened-array
identity/count and mesh buffer dimensions.
The caller invokes it only for actors requested through the API. Warm Spawn
copies local/world/previous-world transforms, bounds and flattened-bone poses;
dynamic geometry copies its separate CPU buffer under the source spinlock.
No clone or material allocation occurs on that warm spawn path. Changed dynamic
or skeleton buffer dimensions reject that emission rather than copying beyond
an allocation. The caller owns clone admission, pool limits and retries.

Each ghost owns its scene root and retains its attachment parent while attached.
Expiration, cancellation and pool destruction hide and detach the root before
releasing either owner. Reuse selects only inactive slots; it does not erase a
previous caller's still-visible ghost. Controllers, collision objects, user-data
and extra-data are removed from detached clone nodes. Auxiliary lights,
particles, overlays, invisible surfaces and decals stay hidden. Meshes retain
their native vertex buffers and skin, while materials use the same ordinary
lighting variant for both originally lit and effect-shaded surfaces.

## Unified silhouette materials (1.0.2)

`Materials.ixx` owns material normalization and the actor pool's cutout cache;
`Coverage.ixx` owns the engine-independent D3D11 conversion and memory admission.
Geometry owns the frozen scene and a detached, independently cloned fade-node
controller per ghost. The controller outlives that ghost's root. It is never
attached to the scene, so distance/animation updates cannot overwrite its fade.
The frame owner supplies `alpha * remainingLifetimeFraction` to its native fade
channels. Materials keep alpha 1 and disable blending. Screen-door fade and
depth writes replace additive accumulation of all visible layers. Material
subclasses, texture RGB, vertex colours/alpha, normal maps, skin/hair tints,
environment/refraction/glow effects and author opacity are not inherited.

SE pass construction (`0x1412C6200`) selects accumulation hint 10 for an opaque
material with fade below 1 and `kScreendoorAlphaFade`. Lighting geometry setup
(`0x1412F2BB0`) selects the native fade stencil state from that controller's
fade. The corresponding flag/hint branch was also inspected through the AE
1.6.1170 and VR 1.4.15 lighting-property vtables. Fade members use CommonLib's
SE/AE/VR runtime accessor. The source-texture factories were decompiled on all
three runtimes; native renderer texture initialization was checked against SE
`0x140D6DBC0`. No new renderer hooks or replacement game shaders are installed.

Cutout textures use independent RGBA8 GPU copies: white RGB, original alpha at
every source mip. Original UV transforms and clamp modes remain. Authored alpha
test thresholds remain; blended-only surfaces use nonzero coverage as a cutout.
An actor's unchanged textures are retained when unrelated scene attachments
change, and unused entries are pruned against the new source graph. Cache
entries retain their source texture; streaming SRV replacement invalidates the
scene bindings and replaces that entry. Reset, unloading and idle eviction
release the masks through native NiSourceTexture ownership. A shared game-thread
budget defaults to 512 MiB (`MaxCoverageMiB`) for retained mask allocations.
Move-only leases return admission when their native texture owner is released;
allocation, unsupported-view and admission failures identify the affected surface.
Driver-retained in-flight commands may outlive those CPU owners temporarily.

The embedded compute shader runs only during cold preparation. Commands are
recorded on an isolated deferred context and executed under the engine renderer
lock with immediate-state restoration enabled. There is no readback, explicit
GPU wait, engine geometry work on a worker, or image file written into Data.
Shader bytecode is generated from the checked-in HLSL with Windows SDK fxc;
xmake checks it before module dependency discovery.

`CoverageTests` imports the production module and executes the shader on WARP
and, with `--hardware`, a real adapter. It checks all pixels/mips, odd dispatch
dimensions, nonzero mip starts, BC3 sRGB source alpha, unchanged source pixels,
restored graphics/compute state, typed rejection and shared-budget lifetime.
These checks do not establish the in-game visual result, native resource teardown
or compatibility with Community Shaders/TAA/VR. Evaluate the original reported
hair, boots, body/hand boundaries and eye/jaw visibility in a new game session,
then exercise repeated reuse, 3D changes, cell changes and loading a save.

## Rim-lighting initialization and first visible spawn

The SE crash recorded on 2026-10-24 at 19:02:18 occurred in lighting material
setup at `SkyrimSE.exe+0x12F27E5` (IDA `0x1412F27E5`). The preceding instructions
load `[material+0x60]`, then dereference its renderer data at `+0x48`. The exception
registers show a null texture, matching `BSLightingShaderMaterialBase::rimSoftLightingTexture`.
The heap holding the material is absent from the supplied minidump, so the dump
alone cannot establish that material's ownership. CrashLogger reports a property
with the rim flag set. The preceding successful API requests were `Prepare`
operations; they had not attached visible silhouettes.

SE `BSLightingShaderProperty::FinishSetupGeometry` (`0x1412C5470`, body
`0x1412C56D0`) passes the existing rim/soft/back-light flags into
`BSLightingShaderMaterialBase::ReceiveValuesFromRootMaterial` (`0x1412CF660`).
That method supplies a default texture for an empty rim/soft slot only when
one of the corresponding flags is already set. Version 1.0.0 enabled rim later
in `Ghost::ApplyLook`, after geometry setup. A source surface without a rim map
therefore reached the renderer with the flag enabled and its texture null.
The old Dodge code enabled the flag before its native setup calls.

Version 1.0.1 provisions rim resources on every detached pooled surface during
`MakeSurface`, before `FinishSetupGeometry`. A surface still missing that texture
is hidden with a specific warning. Appearance changes use CommonLib's `SetFlags`
to invalidate cached render passes when toggling rim, retaining the prepared
texture across uses. The SE setter (`0x141291C30`) resets `lastRenderPassState`
to `INT32_MAX` when the flag changes. No additional cloning or texture loading
is needed for an appearance change at spawn.

Runtime regression checks still required on SE, AE and VR:

1. Prepare a slot from equipment without rim lighting, then trigger the first
   perfect-dodge Spawn with the default positive rim power. Prepare must remain
   invisible; Spawn must render and fade without a lighting-material crash.
2. Reuse that slot with rim power `0`, then positive, then `0` again. Confirm
   both the visible effect and the shader variant update on each transition.
3. Repeat with changing equipment and embedded arrows, both with and without
   Community Shaders. Missing resources must omit and identify the affected
   surface rather than attach it with a null required texture.

These are manual engine checks, not checks covered by the mailbox/timeline unit
tests. A successful DLL build does not verify them.

## Asynchronous alternative and remaining validation

The inspected SE BGSArtObjectCloneTask vtable leads to `0x14020A030`, which
calls a base-form Clone3D virtual and stores the returned model. CommonLib's
BSModelDB::Demand loads a model by asset path. Neither inspected contract
establishes a safe asynchronous snapshot of a live actor's complete animated
geometry. Moving the synchronous actor clone onto an arbitrary worker would
add an unverified scene/renderer race. The implementation therefore creates
cold slots on the game thread under the request owner's per-frame admission
budget and reuses them for subsequent emissions. A cold native clone remains
an indivisible engine call and can still cause a frame hitch; no hard latency
guarantee is claimed. Prepare allows callers to pay that cold cost in advance.

In-game checks are still required for all three executables: rendering and
dynamic mesh uploads, current-pose skinned snapshots, equipment/morph changes,
first-person camera variants, cancellation, cell changes, load/new game, long
sessions, native resource reclamation and coexistence with other clone hooks.
Static analysis does not prove recovery from an engine access violation or a
corrupt third-party mesh; the typed failure paths cover failures the engine
returns and incompatible source/clone relationships detected before attachment.
