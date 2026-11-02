# C++ Engineering Instructions

These are the default engineering rules for this repository. Apply them to all new and changed first-party C++ code, tests, build logic, and relevant documentation. Existing violations are not examples to copy. Follow more specific `AGENTS.md` instructions where present; identify material conflicts instead of silently weakening these rules.

Prefer **correct domain modeling, clear ownership, cohesive architecture, readable source, and verified behavior** over minimal diffs, artificial abstraction, or passing a formatter. Preserve user changes and existing intended contracts. Do not reinterpret a small task as permission for an unrelated rewrite.

## Start with the responsibility and contract

- Read the relevant README, architecture notes, build configuration, component documentation, tests, and current implementation. Old plans and obsolete examples do not override working contracts.
- Before a substantial change, identify the owner of each rule and mutable state, external input boundaries, valid states, possible failures, consumers, lifetime constraints, and the smallest coherent implementation. Use a brief plan; ordinary fixes need no ceremony.
- Put each domain rule and its state transitions under one clear owner. Keep domain decisions independent of transport, serialization, persistence, rendering, OS, and presentation adapters where the project has such layers.
- Keep implementation details **unexported and encapsulated**, not necessarily in separate physical files. A small change should normally be localized to its owning component. Genuine cross-boundary contracts can require coordinated changes; do not duplicate logic merely to reduce the file count.
- Favor high cohesion and explicit dependencies. Avoid generic `Manager`, `Service`, `Common`, or `Utils` dumping grounds, needless forwarding layers, cyclic dependencies, speculative plugin systems, and abstractions built for imagined future requirements.

## Make assumptions explicit and define success

- **Do not silently make material assumptions.** Investigate the existing code and contracts first. Surface remaining uncertainty, competing interpretations, and consequential tradeoffs before committing to a design. Ask for clarification only when the ambiguity cannot be responsibly resolved from available context.
- **Challenge flawed premises.** Suggest a simpler or more accurate alternative when it better satisfies the actual goal. Do not blindly implement a requested mechanism if a substantially better solution exists. Explain the tradeoff briefly and proceed with justified choices.
- **Define observable success for nontrivial tasks.** Establish the behavior, invariants, regressions, and integration checks that demonstrate completion before implementation. Iterate until the criteria are verified, or explicitly report what remains unverified and why. Keep plans and checkpoints proportional to the task.
- **Prefer the smallest coherent solution, not merely the smallest diff.** Avoid unrequested features, speculative configurability, and unnecessary abstractions. Do not use "surgical changes" as an excuse to preserve demonstrably flawed architecture within the agreed scope. Conversely, do not expand the scope to unrelated cleanup.

## Continuously examine and improve the architecture

**Architecture is ongoing engineering work, not a one-time plan written before coding.** Do not just add another field, constructor parameter, forwarded dependency, branch, or callback to make the next test pass. At natural checkpoints, inspect whether the code is signaling a missing or misplaced abstraction. Pause and redesign locally when the evidence warrants it.

- **Before implementing a substantial feature**, identify its domain concepts, state owner, lifecycle, entry points, dependencies, failure semantics, expected extension/variation points, and composition boundary. Sketch the simplest credible structure, including which types/modules own decisions and which only adapt dependencies. An ordinary local fix requires no formal architecture ceremony.
- **During implementation**, reassess after the first vertical slice and whenever new complexity appears. Do not treat the initial design as immutable. If the proposed structure is deteriorating, make a focused architecture correction **before** layering on more workaround code; update affected tests and callers together.
- **Architecture review triggers:** a type accumulating unrelated reasons to change; deeply nested branching on a growing set of states; multiple components duplicating the same coordination; layers that exist only to forward constructors or methods; the same dependency threaded through several unrelated constructors; an expanding collection of booleans/options representing a lifecycle; pervasive runtime type checks, casts, or strings encoding dispatch; event subscriptions with unclear lifetime; or every new feature needing edits to many unrelated modules. These are signals to investigate, **not automatic numeric refactoring thresholds**.
- For each meaningful trigger, identify the *underlying responsibility or coupling problem*, not merely the line count. Consider at least a simple local alternative and one structural alternative where appropriate. Choose deliberately, recording in a short note/commit explanation what changed and why; if the simpler existing structure remains best, state why and proceed. Do not turn routine code into design-pattern theater.
- **Prefer explicit dependency injection at a composition root** when multiple consumers need replaceable services or dependencies are assembled by the application. Use simple constructor injection when it stays clear. If a dependency is only forwarded through unrelated layers, consider relocating construction to the owning composition root, splitting responsibilities, or introducing a coherent capability/facade with a real contract. A DI framework/container is optional and only justified when it demonstrably improves lifetime/scope/composition management; avoid global service locators and hidden resolution in domain code.
- **Use factories** when construction has real choices, invariant checks, resource ownership, multi-step initialization, or variants: named creation functions, typed results, simple factories, or abstract factories as appropriate. Do not build a generic factory hierarchy for one trivial constructor; do not use a factory to conceal unsafe partially initialized objects.
- **Use explicit state machines or variant-based states** when multiple phases and transitions must stay valid together. Prefer strategy/policy objects or compile-time polymorphism where genuinely varying behavior is being repeatedly selected by conditionals. Keep each transition and its side effects under the correct owner.
- **Use events/observer or message-based coordination** when producers and consumers genuinely need to be decoupled. Define typed events, their construction/validation (an event factory only if useful), delivery guarantees, ordering, dispatch context, failure handling, and subscription lifetime. Do not introduce an event bus to obscure ordinary direct synchronous calls or turn requirements into global broadcasts. Separate domain events from infrastructure notifications when they have different contracts.
- Use adapters/ports when crossing real infrastructure or platform boundaries. Use dependency inversion to keep policy independent from I/O details, not to create an interface for every class. Use a mediator only for real coordination complexity, not to replace obvious collaboration with indirection.
- **Restructure before scaling complexity:** where a clean architectural adjustment makes the next changes substantially simpler, implement it as a reviewable step rather than endlessly extending a monolithic type, adding pass-through constructors, or scattering conditionals. Do not invent speculative flexibility for imaginary future needs. In performance-sensitive code, evaluate dispatch, allocation, and lifetime costs of proposed abstractions.
- If a meaningful redesign affects a wider public contract, ABI, serialization format, or unrelated owners, describe the issue, alternatives, compatibility effects, and incremental migration plan before proceeding. Implement safe, authorized local improvements without waiting for routine approval; do not silently perform an unrelated repository-wide rewrite.
- **Architectural acceptance:** after implementation, review how the responsibilities, dependencies, event flow, state transitions, and ownership boundaries actually evolved. Ask whether the chosen structure would make the *next realistic feature* easier without introducing unnecessary layers. Tests and formatters do not prove that architecture is sound; review final call sites and module dependencies.

## C++ modules first; colocate API and implementation by default

**Use standard C++ named modules (C++20 or later) for new first-party components wherever technically feasible.** The default is **one cohesive module interface source containing its public contract, private declarations, and function/method definitions together**, like a C#/F#/Java source file. A module is an encapsulation/compilation boundary, **not a mandate to recreate the header-versus-implementation split**. Prefer `import` to first-party `#include`.

- Use a **primary module interface unit** (`export module project.component;`). Export only what consumers need; keep private members, internal helpers, and implementation alongside the relevant API in the same source file. `export` controls visibility to importers; file separation is not required for hiding implementation.
- **Do not routinely create `module project.component;` implementation units or separate `.cpp` files for methods exported from a module.** Do not declare a class in one file only to define its ordinary methods in another, nor split an otherwise cohesive module into public/private physical halves by convention. A separate module implementation unit requires a documented practical justification, such as a demonstrable build/dependency constraint, unavoidable integration boundary, or genuinely independently owned large implementation.
- A large responsibility should be divided into **cohesive modules** or selectively into module partitions, not kept as an enormous file merely to satisfy colocation. Partitions are for real internal or public sub-boundaries and coherent capabilities, never a default way to hide all implementations. Prefer several cohesive modules over a giant module plus a pile of implementation units. Avoid gratuitous one-method modules, re-export chains, and cyclic import graphs.
- Keep related declarations, implementations, private helpers, and invariants discoverable together. Prefer method definitions next to their declarations when readable; if an out-of-class definition is genuinely clearer, it may appear later **in the same module interface unit**. Preserve C++ language requirements for declaration order, ODR, linkage, and template reachability.
- Modules do not replace namespaces, type-level encapsulation, or good API design. Avoid exporting implementation-only names, third-party/platform declarations, and details consumers do not need. Import dependencies explicitly; re-export only when it is part of the intended public contract.
- For templates, concepts, inline and constexpr functionality, ensure definitions and constraints are available where required for instantiation and use. Do not place needed definitions in inaccessible implementation-only translation units.
- Prefer `import std;` **only when the actual compiler, standard library, language mode, and build system support it reliably**. Otherwise use the repository's verified standard-library inclusion strategy. Module imports do not provide preprocessor macros.
- For necessary C/SDK/third-party headers, use a narrowly scoped **global module fragment** (`module;` with required `#include`s/configuration macros, followed by `export module ...;`) when appropriate. Avoid textually including legacy headers in a named-module purview without considering attachment, linkage, and ODR. A header unit is not a guaranteed substitute for macro-configured headers.
- Do not export macros as first-party API. Prefer typed constants, enums, templates, concepts, and ordinary functions. Preserve macros only at real preprocessing, C, SDK, or platform boundaries.
- Traditional headers or non-module `.cpp` units are exceptions for **verified interoperability constraints**: stable C/header-based ABI, external clients unable to import modules, generated/third-party SDK APIs, macro-driven or platform-specific integration, and unsupported build/toolchains. Check whether a global module fragment or an adapter module is sufficient; localize and document any remaining traditional source. A familiar style or speculative compile-time concern is not justification.
- A **private module fragment** (`module :private;`) is optional, not the default. It still keeps the code in one source file, but its restrictions and compiler support must be checked; it is not needed merely because a method or helper is private.
- Do not convert unrelated existing code mechanically in a focused task. When working on a component with traditional headers, prefer a planned incremental migration, preserving consumers and ABI. Do not globally abandon modules because one external boundary needs headers.
- Match source extensions (`.ixx`, `.cppm`, `.mpp`, etc.) and dependency scanning, interface compilation, BMI/IFC ownership, and import ordering to the actual toolchain. Never commit compiler-generated BMI/IFC artifacts as portable sources.
- Verify a **clean build**, relevant **incremental build**, and a real importing consumer when creating or changing a module boundary. Check applicable configurations and supported compilers. Address dependency/ODR failures directly rather than silently falling back to blanket includes.

Preferred single-file example (adjust style and standard-library imports to the project):

```cpp
// Counter.cppm — API and implementation together
export module project.counter;

export namespace project
{
    class Counter
    {
    public:
        explicit Counter(int initial) noexcept
            : value_(initial)
        {
        }

        [[nodiscard]] int Value() const noexcept
        {
            return value_;
        }

        void Reset() noexcept
        {
            value_ = 0;
        }

    private:
        int value_;
    };
}
```

This is a colocation example, not an endorsement of trivial wrappers, a mandatory naming style, or blanket `noexcept`. Non-exported helpers and exported classes can coexist in the same module file. Keep functions cohesive and readable rather than forcing every substantial function body inline inside a class.

## Design APIs to guide correct use

- **Review APIs from the caller's perspective.** A public interface should reveal what a consumer can do, which states and inputs are valid, what ownership and lifetime it requires, what it changes, and how it reports failure. Examine real call sites rather than judging an API only by its declaration.
- **Make correct usage easy and incorrect usage difficult (poka-yoke).** Express meaningful distinctions through types, constrained construction, named operations, and explicit state transitions. Avoid ambiguous boolean flags, loosely related setters that permit inconsistent intermediate states, raw stringly typed protocols, and public representation details that bypass invariants. Do not introduce wrappers or elaborate type hierarchies without a real misuse risk or domain benefit.
- **Apply Command-Query Separation (CQS) where it clarifies contracts.** Queries should normally observe without changing externally visible state; commands should make their effects explicit. Do not force a split when one atomic operation intentionally both changes state and returns a result (for example, `TryPop` or an idempotent state transition). Name such operations to communicate their semantics.
- **Design for discoverability and clear affordances.** Use domain vocabulary, intention-revealing operation names, typed units, and coherent method groups. Prefer interfaces that advertise valid actions over a broad surface of low-level mutation knobs. Comments document non-obvious constraints and rationale; they should not compensate for misleading names or unenforced invariants.
- **Verify API ergonomics and evolution.** Exercise typical and invalid usage at representative call sites and in contract tests. Review compatibility, error semantics, ownership, allocation, and dispatch costs at module/ABI boundaries. Do not add public surface area, extension points, or generic interfaces merely for hypothetical consumers.

## Model valid states with types

- Make invalid domain states unrepresentable **where practical**. Use meaningful strong IDs, scoped enums, constrained construction, aggregates with valid invariants, `std::variant`, and explicit state transitions instead of unrelated booleans, null sentinels, and mutually inconsistent optional fields.
- Use `std::optional<T>` for *legitimate absence only*. Missing required input, malformed input, unsupported operations, rejection, cancellation, and infrastructure failures need explicit, distinguishable outcomes. Do not manufacture an empty value or `nullopt` after a failure.
- Prefer `std::expected<T, E>` when C++23 and the toolchain support it; otherwise use the existing project-approved typed outcome mechanism. Return a plain value from an operation that cannot fail for valid inputs. Do not wrap every internal call in `expected` or revalidate invariants already guaranteed by construction.
- Establish invariants through owning factories/parsers. Keep untrusted deserialization, casts, sentinels, default-initialized invalid values, and unchecked conversions out of trusted domain types. Account for valid/invalid default construction explicitly.
- Keep distinct meanings distinct: absent, not requested, unchanged, explicitly cleared, pending, rejected, unavailable, canceled, and committed. Prefer a named state type when nested flags/optionals become unclear.
- Handle closed domain variants exhaustively. Do not add a default case that hides a new alternative. Handle unknown external enum/wire values at their own compatibility boundary.
- Use templates, concepts, `constexpr`/`consteval`, and other compile-time mechanisms to express **real** shared contracts, reduce present duplication, or eliminate real runtime overhead. Favor understandable constraints and compiler diagnostics. Do not replace a few coherent functions with metaprogramming machinery merely to maximize reuse.

## Validate once at the owning boundary

- Parse external data into checked domain values at the boundary that owns each invariant. Reuse that constructor/parser; do not spread slightly different checks across callers, adapters, storage, and UI.
- **Stable value invariants** are established at construction; **current-state preconditions** (permissions, versions, existence, phase, operation IDs, capacity) are checked by the current owner when applying the operation. An earlier check can become stale. Never remove a necessary current-state check under the slogan “validate once.”
- Validate binary formats, lengths, offsets, arithmetic overflow, array bounds, enum ranges, and platform/ABI inputs before trusting them. Reject invalid input before committing business state or reporting success.
- Choose size limits, quotas, timeouts, and resource budgets from format contracts, real constraints, or measured workloads. Name units and explain purpose. Avoid arbitrary defensive caps, silent clipping/truncation, and duplicated policies that unexpectedly reject valid use cases.
- Preserve clearly defined commit points and partial-failure semantics. An asynchronous timeout or transport failure does not prove that an accepted state-changing operation never executed. Do not add automatic retry without an idempotency/settlement contract.

## Own mutation, resources, and concurrency

- Mutable state is allowed inside a well-defined, serialized owner (actor, thread, transaction, subsystem, or other existing model). Do not force persistent immutable reconstructions where owner-confined mutation is simpler and faster. Do not introduce actors, locks, or thread pools for their own sake.
- Publish immutable value snapshots or stable borrowed views with an **explicit lifetime contract**. `const` alone does not make aliased mutable storage immutable; a `std::span<const T>` or `std::string_view` does not own data. Do not expose mutable aliases or lazy/live views across ownership boundaries.
- Give every resource a clear owner: RAII by default, values and `std::unique_ptr` for exclusive ownership, `std::shared_ptr` only for genuine shared lifetime, borrowed pointers/references/views only when their validity is established. Avoid global mutable singletons and ownership cycles.
- Preserve construction/destruction order, lock scope, iterator/reference invalidation, move semantics, callback registrations, handles, buffers, and external API lifetimes when extracting helpers or changing control flow. Handle partial initialization, cancellation, errors, and shutdown consistently.
- Detached background work receives owned/stable inputs and returns through the owning synchronization boundary. Check the current generation/session/operation before applying late completion. Cancellation must not be assumed to roll back an already committed effect.
- Bound queues, outstanding operations, caches, retained results, and background work independently where they can grow. Define admission, backpressure, dropping/coalescing, and shutdown semantics. Do not silently lose reliable operations or block the owner while waiting for work that requires that owner to complete.
- Preserve established threading and platform contracts. Never move UI, GPU, engine, COM, or other thread-affine operations to a worker without proof the API permits it. C/C++ ABI callbacks and foreign boundaries must not leak exceptions or borrowed objects beyond their valid lifetime.

## Errors are values; exceptions are exceptional boundaries

- **Do not author exception-based control flow in production project code.** Expected absence, invalid input, failures, rejected commands, missing files, and unsupported operations return typed outcomes. Do not introduce `throw`, exception-based guard helpers, or deliberately throwing accessors for normal failures.
- Prefer nonthrowing/status APIs (`std::error_code` overloads, `from_chars`, `try_` functions, project-specific results) where available. Do not branch on human-readable error text or collapse a failure into success, `nullopt`, zero, or an empty collection.
- Third-party and standard-library operations that report ordinary failures only by throwing may be caught in a **narrow adapter**. Translate the relevant exception into a typed outcome once; preserve its cause, error category, and resource cleanup. Do not wrap an entire business workflow in an indiscriminate `catch (...)`.
- Top-level process/service supervision may contain unexpected exceptions only when it has a concrete isolation, shutdown, cleanup, and recovery policy. Do not swallow internal faults or continue operating on potentially inconsistent state. Do not claim that catching all exceptions makes native code recoverable.
- Do not add catch/rethrow chains or exceptions for routine allocation concerns. Do not mechanically add `noexcept`: apply it only when it matches the actual contract. An exception leaving a `noexcept` function terminates the process; do not disable exception support as a substitute for correct error handling.
- When changing an existing exception boundary, trace all relevant callers and partial effects first. Do not remove a catch and accidentally turn an ordinary dependency failure into termination. Keep expected rejection and unexpected owner/lifecycle failure distinguishable.

## Design for performance without sacrificing clarity

- Identify hot paths from the actual workload (frame loops, I/O, rendering, networking, parsing, storage, etc.). Inspect allocations, closure captures, type erasure, virtual dispatch, boxing at foreign boundaries, temporary containers, string conversions, copying, cache locality, and unnecessary whole-dataset scans.
- Choose `std::vector`, associative containers, contiguous/flat containers, stable-handle containers, and custom allocators according to iteration, ordering, invalidation, ownership, and measured access patterns; not fashion. Avoid defaulting to node-based containers for every lookup or replacing clear types with raw primitives for speculative speed.
- Favor direct loops, ranges, algorithms, templates, and noncapturing callbacks as appropriate. In hot paths, inspect unintended heap allocations and the lifetime of closures rather than assuming `inline` or templates make a call allocation-free.
- Use views (`std::span`, `std::string_view`) for bounded borrows, never for unowned asynchronous storage. Move owned values when ownership transfers; avoid unnecessary copies, but do not trade safety for micro-optimization.
- Keep performance-sensitive diagnostic work outside production paths. Support material performance claims with measurements or generated-code evidence on equivalent workloads; do not demand a benchmark for every straightforward change.

## Manually maintain readable source and cohesive files

- **Formatting tools are not code organization.** After any `clang-format` run, read the final source and manually improve semantic grouping, declarations, control flow, and responsibilities. Do not declare a readability task complete because formatting or tests passed.
- Leave blank lines between independent functions, types, and meaningful member groups. Within nontrivial functions, separate coherent phases—prepare, validate, look up, compute, mutate, invoke external effects, handle results—as the actual logic requires. Keep closely related statements together; do not insert mechanical blank lines after every statement.
- Expand dense conditions, nested branches, calls, initializer lists, switch/case bodies, chained expressions, and multi-operation lines when compact presentation hides meaning. Name intermediate decisions when useful; do not add pointless temporaries for their own sake.
- Reduce deep nesting with idiomatic guards, explicit cases, and named cohesive helpers. Break up oversized functions/types/files **when they mix responsibilities**. A large cohesive unit is acceptable; dozens of trivial forwarding helpers and indiscriminate splitting are not.
- Keep each operation discoverable near its owner. Prefer colocated module contracts and implementations rather than making readers jump between declaration and implementation files. Do not scatter one state transition across unrelated modules or classes.
- Use descriptive names and named constants for meaningful units, thresholds, masks, sizes, policy settings, and intervals. Keep constants near their owning subsystem or in a coherent shared module, not one global dumping ground.
- Preserve repository syntax style (`.clang-format`, established conventions, and user edits). A full-format sweep is not a substitute for manual work and is not appropriate during an unrelated task. Avoid blanket regex whitespace rewrites or automatic “cleanup” that obscures intentional structure.
- Favor focused comments explaining *why*, invariants, lifetime constraints, or non-obvious external behavior. Avoid commented-out implementations, obvious line-by-line narration, and section banners covering unreadable functions.

### Acceptance for an explicitly requested broad readability/refactoring pass

- Review the full agreed scope of **handwritten** module files (including colocated implementations), necessary traditional headers/sources, tests, and examples. Maintain a finite inventory of reviewed files, confirmed findings, dispositions, accountable owners, and verification. Do not mistake an empty self-selected work queue for complete coverage.
- For delegated edits, assign exact files, approved examples, invariants, forbidden semantic changes, checks, and output evidence. Use one writer per file and one integrator. Review each modified function in its full final context; significant structural changes should receive independent qualified review when possible.
- Check module boundaries, exported APIs, and architectural responsibility changes after decomposition. Preserve evaluation order, RAII scope, failure paths, memory/lifetime semantics, and concurrency ownership. Verify source after formatting rather than only checking a diff.
- Show representative real before/after examples and report remaining exceptions explicitly. Whitespace-only changes need no invented behavioral tests; structural changes require checks appropriate to their actual risks.
- These inventory requirements apply to an expressly requested broad pass; routine edits require focused review of the touched behavior and contracts.

## Remove dead code and compile out optional diagnostics

- Remove **confirmed** unused implementations, obsolete scaffolding, unreachable branches, stale compatibility shims, and temporary probes. Remove orphaned tests/configuration/exports/imports/build rules/docs as appropriate. Do not preserve commented-out code “just in case.”
- Before deletion, check indirect use through registration, reflection, generated bindings, C ABI exports, plugin entry points, serialization, tests, macros, build modes, and external consumers. No local call sites does not prove that a symbol is dead.
- Optional development-only traces, verbose dumps, profiling, and test instrumentation must be compiled **only under explicit build flags**, disabled in the default production configuration. Reuse project conventions; if there is no such mechanism, add a small documented build-time option as needed.
- Gate diagnostic implementation **and call sites**, including argument formatting, allocations, capturing, probes, callbacks, and registrations. A disabled runtime logger that still evaluates costly arguments is insufficient. Remove diagnostics that no longer provide value rather than retaining them all behind flags.
- Keep essential production logs, errors, security/safety checks, invariant validation, cleanup, and supervision available without diagnostic flags. Verify the relevant diagnostics-enabled and diagnostics-disabled builds; do not claim production exclusion from a debug-only build.

## Tests, build integration, and documentation

- Find the repository's actual build, test, sanitizer, formatter, static-analysis, and deployment commands; do not invent generic scripts or assert support for a tool that is not configured. Use the smallest relevant checks first, then appropriate integration/full-suite checks for affected boundaries.
- For public module API changes, compile relevant consumers. For module changes, verify dependency scanning, generated BMI/IFC handling, and clean/incremental builds in supported configurations. When architecture changes, inspect the final dependency graph and composition root as well as the code that invokes the new abstraction. Keep source-of-truth build files synchronized; never commit intermediate module artifacts as sources.
- Add focused regressions for changed behavior and discovered bugs. Test observable contracts, valid/invalid states, errors, cancellation, late completion, RAII cleanup, and version/compatibility behavior where applicable. Avoid assertions that merely repeat implementation details.
- Verify the **tests themselves**: correct expected outcomes, causal synchronization, meaningful assertions, and real failure observation. Fix defective tests with reasons; do not weaken checks, blindly add sleeps/retries, accept zero selected tests, or rerun until lucky.
- Fix concrete bugs discovered while working in scope, when the intended contract is established and the fix stays reviewable. Revisit the architecture when a bug reveals misplaced state ownership, duplicated coordination, or missing construction/lifecycle boundaries. Preserve intended behavior, not demonstrably incorrect behavior. Separate behavioral fixes from manual formatting where practical; record larger independent findings rather than silently expanding a small task.
- Update affected documentation and examples in the same change whenever APIs, architecture, ownership, modules, build requirements, failure behavior, flags, or configuration change. Edit generated files through their actual source definitions/generators. Keep persistent docs concise and current rather than turning them into run logs.
- Preserve the user's working tree, follow repository authorization for commits, and never push, merge, tag, publish, delete user data, or alter live environments without explicit permission. Report what changed, why, the checks actually run and their results, and what remains unverified or blocked. Missing tools/fixtures are limitations, not passes.
