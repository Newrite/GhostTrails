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
material identities, flattened-array identity/count and mesh buffer dimensions.
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
particles, overlays, invisible surfaces, weapon blood and rigid decals stay
hidden. Skinned decal meshes remain eligible: the decal flag alone does not
establish that a mesh is an auxiliary overlay. Rigid effect geometry is omitted;
skinned effect-shaded clothing remains eligible. Native lighting
materials remain lighting materials; effect-shaded armor is rendered through a
fresh lighting property, retaining its native mesh and skin.

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

## Rendering regression and native material revision (1.0.3)

This section records the 1.0.3 implementation. Its per-material fade and
Cutout policy are superseded by the 1.0.4 section below.

The user observed a visible ghost in 1.0.1, with an overly bright body and faint
hair/boots. The old screenshot shows hair around the whole head; no missing
hair patch was established. In 1.0.2 the ghost disappeared and vegetation turned
white for the effect's lifetime. GPU conversion tests did not exercise Skyrim's
render integration. The exact interaction causing that regression has not been
isolated, and no compatibility conclusion about Community Shaders/ENB follows
from those tests.

Version 1.0.3 removes the direct D3D11 conversion, shader bytecode, GPU mask
cache/budget and private screen-door fade path. `GhostTrails.Materials` owns
native surface construction and per-surface opacity, using the 1.0.1 path and
retaining its rim initialization repair. Each property receives a unique native
material copy; an unexpected alias to a source/default material rejects that
surface before modification. Alpha properties are owned by the cloned geometry.
The runtime and API no longer carry a GPU coverage budget.

Surface blending uses SrcAlpha/InvSrcAlpha instead of SrcAlpha/One to reduce
additive overbrightness. Opaque meshes still use the native white texture;
alpha-tested/blended meshes keep their source diffuse texture, including its
colour and alpha. Effect-shaded skinned clothing uses its source texture on the
new lighting material. This deliberately avoids costly hair-colour normalization.

After native SetupGeometry derives vertex-input flags, the ghost property clears
VertexColors and VertexAlpha before FinishSetupGeometry. The inspected lighting
shader multiplies diffuse alpha by material alpha and, when vertex colours are
enabled, vertex alpha. Ignoring that vertex input removes an extra source of
opacity reduction without editing vertex buffers.

The same shader tests the resulting alpha against the property's 8-bit alpha
reference. For the usual greater/greater-equal hair cutout, a fixed reference can
reject even fully opaque texels as material alpha fades. `Coverage::Cutout`
scales the authored reference by current opacity, rounding down to a byte.
This retains originally accepted texels during fading, with less than one byte
of reference quantization. It preserves the source test mode and does not turn
soft texture alpha into a binary mask. Other authored comparison modes keep
their native comparison; complete self-occlusion is not provided by this blend.

`GhostTrails.MaterialTests` exercises surface classification and the cutout
reference across all 256 authored reference values and 65 fade samples. These
CPU checks do not verify native material setup, pass ordering, hair/boot density,
internal facial surfaces or vegetation in the game. All still need runtime
verification on the new DLL.

## Native object fade and hair tint (1.0.4)

The user's 1.0.3 screenshots confirm improved body/armor visibility but show
mouth and eyes through the back of the head. The supplied log confirms the
head, mouth, eyes and Dint HairTint surfaces were captured. CommonLib's
NiAVObject::UpdateHairColor writes BSLightingShaderMaterialHairTint::tintColor.
Surface::ApplyColor now performs that same assignment on the uniquely cloned
material; it does not edit actor/NPC data or source textures.

The ordinary engine object-fade route was inspected in all three binaries:

| Runtime | Lighting GetRenderPasses | Transparent rendering stage | Fade control byte |
|---|---|---|---|
| SE | `0x1412C6200` | `0x1412CB2E0` | node `+0x154` |
| AE 1.6.1170 | `0x1414ADFB0` | `0x1414B3390` | node `+0x154` |
| VR | `0x1413045F0` | `0x1413099C0` | node `+0x17C` |

With currentFade below one, explicit alpha blending disabled, screen-door
disabled and the private node's alternative-fade byte zero, the lighting
property emits colour hint 9 and Utility depth hint 14. SE accumulation
`0x1412CA300` routes these to groups 7 and 11. All three rendering stages
draw group 11 before group 7. SE alpha-state setup `0x14131F440` enables
ordinary colour blending from property alpha even with the explicit blend bit
off. Setting alpha to one would instead choose opaque rendering; the plugin
maps that endpoint to the next smaller float (difference below 0.0000001).

SE state construction `0x140D88C90` identifies depth mode 3 as write-enabled
LESS_EQUAL and mode 1 as read-only LESS_EQUAL. The ordinary opaque stage leaves
mode 3 for the depth group; colour group 7 selects mode 1 and restores mode 3.
The special render flag 0x40 can leave mode 1 before the depth group. Therefore
ordinary world ordering is supported by static evidence, but all camera modes
and third-party render changes are not proven. This is shared scene depth,
not a per-ghost depth target: overlapping ghosts and later alpha effects can
occlude each other. No engine globals or graphics API state are edited here.

The native empty fade-node prototype is cloned once per pooled ghost. It stays
detached, with a NiPointer owner declared before the graph so every shader
borrow dies first. CreateFadeControl rejects nonempty/controller-bearing or
aliased prototypes. The alternative-fade byte is set through CommonLib's
runtime accessor, whose field is still named unk154. Its behavior, not a
guessed semantic name, justifies the write. Native AttachChild only changes
parent/child relationships and does not replace these shader fade pointers.

Lighting colour tests texture alpha multiplied by fade; Utility depth does not
multiply texture alpha by fade. The previous scaled positive cutoff would
write invisible depth (e.g. reference 128, fade 0.5, texture alpha 80/255).
The new shared reference is zero: colour retains soft coverage, while native
Utility setup `0x14130EC70` uses approximately (reference+1)/255 for non-blended
depth, excluding empty texels. Blended-only cards are promoted to alpha test
as well. This deliberately admits soft texels below the original authored
cutoff. Tiny filtered alpha below 1/255 can contribute colour without depth;
the shader paths do not provide identical sub-byte edge coverage.

MaterialTests now covers the invisible-depth regression and all nonzero 8-bit
texels over 256 fade samples, including the alpha=1 route boundary. It is a
CPU model of the audited shader contracts, not a render test. Diagnostic builds
log pass hints and fade-node identity once after each spawn. Live validation
must check rear-view head occlusion, hair edges/colour, overlapping ghosts,
vegetation, first-person views, unloading and CS/ENB coexistence.

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

# Frame synchronization update boundary (1.0.7)

The previous task queue boundary was incorrect for a continuous update.
IDA shows that the ProcessTasks call inside Main::Update is under
`if (!gameplayUpdate)`, where gameplayUpdate is `!paused && !freezeTime`.
The other SKSE task queue call is reached from Job_Post_process (SE
`0x140640DB0`), after waiting for job 20. Hooking only the former accumulated
requests during gameplay and processed them on Escape; paused lifetime
advancement then kept the ghosts on screen. This was reproduced in the
user's 1.0.6 log, not a broken hook chain.

Use the unconditional platform update call inside Main::Update after the
job-wait phase instead, on the same thread as Main::Update. IDA confirms:

| Runtime | Function | Offset | Call address | Native target |
| --- | --- | --- | --- | --- |
| SE 1.5.97 | ID 35565 | `0x56D` | `0x1405B355D` | `0x14113E950` |
| AE 1.6.1170 | ID 36564 | `0x9DC` | `0x14064687C` | `0x14124ABF0` |
| VR 1.4.15 | RVA `0x5BAB10` | `0x611` | `0x1405BB121` | `0x141198BA0` |

The pause/freeze branches rejoin before this call. FSMP's MainHooks::Unk_sub
also wraps this call, invokes its previous target, then sends FrameSyncEvent
which waits for physics and writes bone transforms. Install on InputLoaded
and call CommonLib's returned previous target before capturing any pose.
Do not hook either SKSE task queue call, or move scene mutation into its job
callback. Diagnostic mode logs the first tick and pause-mode transitions
with delta and current/main thread IDs. Runtime validation in game is still
required, including compatibility with different FSMP builds.

The old outer MainLoop hook remains removed. Load/revert callbacks clear
owned graphs immediately. Before native shutdown, clear remaining graphs
through the WinMain call site: SE ID 35545 + `0x35` (`0x1405ACC05`), AE ID
36544 + `0x1AE` (`0x14063EB1E`), VR RVA `0x5B4290` + `0x35`
(`0x1405B42C5`). Each is an E8 call in IDA. The previous handler is called
after cleanup; an already installed SafeExit can still terminate normally.
