# wasi-ecs

A small, header-only, compile-time Entity Component System for C++23. No
runtime polymorphism, no type erasure, no virtual calls in the hot path.

## Design

- **Entities** are `u32` indices. An entity must be created with at least one
  component — there's no concept of an empty entity.
- **Components** are plain structs, each with a permanent compile-time ID
  derived from its position in a type list (`comp_type_index<T, List>`).
- **Storage** is a sparse set: one dense `std::vector<T>` per component type,
  O(1) add/remove via swap-and-pop, with an `entity → index` sparse array for
  O(1) lookup.
- **Systems** are CRTP types (`SystemBase<Derived, CMgr, Components...>`) with
  a `constexpr` signature computed from their component list. Active systems
  live in a `std::tuple<Systems...>`, dispatched via fold expressions — no
  `ISystem` interface, no vtable.
- **Archetypes** (`Archetype<CList, Ts...>`) are named component bundles for
  ergonomic batch entity creation. Not a storage strategy — components are
  still stored per-type, not per-archetype.

## Why not EnTT / flecs?

| | wasi-ecs | EnTT | flecs / bevy_ecs |
|---|---|---|---|
| Storage | sparse set per component type | sparse set per component type | archetype tables |
| Component/system IDs | compile-time `constexpr` | runtime, type-erased | runtime, cached |
| System dispatch | tuple + fold expression | none built in | scheduler |
| Multi-component iteration | probe smallest array, lookup rest | same | contiguous (pre-grouped) |
| Runtime type registration | no — fixed list at compile time | yes | yes |

Architecturally close to EnTT's sparse-set core, but trades runtime
flexibility for compile-time IDs and zero type erasure. No archetype-grouped
storage like flecs/bevy: add/remove component is O(1) with zero data
movement here, versus an O(k) move into a different table for archetype
storage — but multi-component iteration pays a random lookup per entity
instead of being fully contiguous. Whether that's a good trade depends on
the workload, not on the genre: it favors components that change
composition often relative to how often wide multi-component scans run; it's
a worse fit if iteration dominates and most entities' component sets are
fixed at spawn. See [Future plans](#future-plans) for archetype-grouped
storage as an option, not a replacement.

## File map

```
include/ecs/
├── common.hpp              Core types, constants, concepts
├── component_registry.hpp  ComponentList<Ts...>, comp_type_index<T, List>
├── component_manager.hpp   ComponentArray<T>, ComponentManagerImpl<Ts...>
├── system_registry.hpp     SystemList<Ts...>, sys_type_index<T, List>
├── system_base.hpp         SystemBase<Derived, CMgr, Cs...> CRTP base, SystemType_t
├── system_manager.hpp      SystemManagerImpl<Ss...>, make_system_manager
├── entity_manager.hpp      EntityManager — ID issuing/recycling, signatures
├── archetype.hpp           Archetype<CList, Ts...> — entity templates
├── world.hpp               World<CList, SList> — the public API
├── game_registry.hpp       YOUR FILE — lists, aliases, GameWorld, schedules
├── components/
│   └── 2d_comps.hpp        Vec2, Transform2, RigidBody2
└── systems/
    ├── movement_system.hpp     implemented
    ├── input_system.hpp        stub, not yet implemented
    ├── render_system.hpp       stub, not yet implemented
    └── collision_system.hpp    stub, not yet implemented
```

**Rule:** library files (`common` → `world`) never include `game_registry.hpp`
or a concrete system header. Dependency flows one way only:
`common → {component,system}_registry → component_manager / system_base →
system_manager → {entity_manager, archetype} → world → game_registry`.

## Quick start

```cpp
#include "ecs/game_registry.hpp"

GameWorld world;

// Every entity needs an initial signature — no empty entities.
Signature sig;
sig.set(GameWorld::component_id<Transform2>);
sig.set(GameWorld::component_id<RigidBody2>);

Entity e = world.create_entity(sig);
world.add_component<Transform2>(e, Transform2{});
world.add_component<RigidBody2>(e, RigidBody2{.v = {1, 0}});

world.update(dt, ALL_SYSTEMS);
world.destroy_entity(e);
```

Or via an archetype, which computes the signature for you:

```cpp
Entity e = world.create_from_archetype<PlayerArchetype>();
// or with initial values, in archetype component order:
Entity e2 = world.create_from_archetype<PlayerArchetype>(
    Transform2{.pos = {0, 0}}, RigidBody2{.v = {1, 0}});
```

## Extending

**`game_registry.hpp` is the only file you edit.**

1. Define the component struct, append it to `MyComponents` (never reorder —
   position is the permanent ID).
2. Write the system as a `SystemBase` CRTP subclass, include its header,
   append it to `MySystems` (position is the schedule bit).
3. The `static_assert`s at the bottom of each list guard against accidental
   reorders.

## Constraints

| Constant | Value | Note |
|---|---|---|
| `MAX_ENTITIES` | 4096 | size of sparse arrays |
| `MAX_COMPONENTS` | 32 | width of `Signature` bitset |
| `MAX_SYSTEMS` | 16 | width of `Schedule` bitset |

Raise `MAX_ENTITIES` in `common.hpp` if you need more; each component array
and system holds one `std::array<u32, MAX_ENTITIES>` sparse table (4KB each).

## Future plans

### Paged sparse arrays

Every sparse array today (`ComponentArray::entity_to_idx`,
`SystemBase::sparse`, `EntityManager::signatures`) is one flat
`std::array<T, MAX_ENTITIES>`, allocated in full regardless of how many
entities actually use it. Planned fix: replace these with a shared
`PagedSparseArray<T>`, EnTT-style — 1024 entries per page, pages allocated
lazily on first write instead of all at once.

This makes raising `MAX_ENTITIES` cheap for component types whose usage
stays clustered (e.g. a setup-only marker only the first N entities ever
get) — verified at 96.7% memory savings in a standalone prototype for that
case. It's a much smaller win (~12% in the same prototype) for component
types that end up touching most of the address space over a long-running
program's lifetime, since scattered access ends up allocating most pages
anyway. Full design, call-site diffs, and a tested standalone prototype
exist; not yet integrated into the core.

### Archetype-grouped storage

Currently `Archetype<CList, Ts...>` is explicitly *not* a storage strategy —
it's a convenience for batch entity creation, while components stay
per-type in `ComponentArray<T>`. A future is to group entities by
their exact component set into per-archetype tables, the way flecs/bevy do.
That makes multi-component iteration fully contiguous (no per-entity random
lookup), at the cost of an O(k) data move whenever an entity's component set
changes. Worth pursuing if profiling shows the random-lookup cost in
multi-component systems dominating over component add/remove cost — not
worth it if the opposite is true. This is a real rewrite of the storage
layer, not a tweak; conceptual only at this point, no detailed design yet.

