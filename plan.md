# ECS Rewrite Plan: Paged Storage + Archetype Storage

This plan covers two rewrites, in the order they should be implemented:

1. **Paged sparse storage** — replace `std::array<X, MAX_ENTITIES>` sparse
   arrays with lazily-allocated 1024-entry pages (EnTT-style).
2. **Archetype-based storage** — replace per-component-type `ComponentArray<T>`
   + per-system sparse sets with archetype tables, so systems iterate
   contiguous, fully-packed rows instead of one direct array + N random
   lookups.

Part 2 depends on Part 1 (the archetype store's entity→location table is
itself a paged sparse array), so do them in this order.

---

## Part 1 — Paged Sparse Storage

### The problem, concretely

Three places in the current code eagerly allocate `MAX_ENTITIES` (4096)
slots no matter how many entities actually exist:

| File | Member | Size today |
|---|---|---|
| `entity_manager.hpp` | `EntityManager::signatures` | `4096 * sizeof(Signature)` |
| `component_manager.hpp` | `ComponentArray<T>::entity_to_idx` — **one per component type** | `4096 * 4 bytes` × `MAX_COMPONENTS` |
| `system_base.hpp` | `SystemBase::sparse` — **one per system** | `4096 * 4 bytes` × `MAX_SYSTEMS` |

None of this is proportional to actual usage. A game with 200 live entities
and 12 component types still pays for `4096 * 12` `uint32_t` slots
(~200KB) it never touches, and `MAX_ENTITIES` becomes a hardcoded ceiling
baked into every `std::array`.

### Design

A `PagedSparseArray<T, PageSize = 1024>` that behaves like a sparse map from
`uint32_t` (Entity) → `T`, backed by lazily-allocated fixed pages instead of
one big flat array. Same O(1) access as today, one extra pointer chase.

**New file: `ecs/paged_array.hpp`**

```cpp
#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

// PagedSparseArray<T, PageSize>
// ==============================
// Sparse map from a dense integer key (Entity) -> T.
// Backed by pages of PageSize slots, allocated lazily on first write to a
// key that falls in that page. Reads to an unallocated page return the
// provided default (never allocate on read).
//
// entity 0..1023   -> page 0
// entity 1024..2047 -> page 1
// etc.
//
// This directly replaces flat `std::array<T, MAX_ENTITIES>` members that
// are indexed by Entity: EntityManager::signatures, ComponentArray::entity_to_idx,
// SystemBase::sparse.

template <typename T, uint32_t PageSize = 1024>
class PagedSparseArray
{
  private:
    using Page = std::array<T, PageSize>;

    std::vector<std::unique_ptr<Page>> pages;
    T default_value;

    [[nodiscard]] static constexpr auto page_of(uint32_t key) -> uint32_t
    {
        return key / PageSize;
    }
    [[nodiscard]] static constexpr auto offset_of(uint32_t key) -> uint32_t
    {
        return key % PageSize;
    }

  public:
    // default_value is what get() returns for keys whose page was never
    // allocated (e.g. INVALID for uint32_t sparse indices, Signature{} for
    // per-entity signatures).
    explicit PagedSparseArray(T default_value = T{}) : default_value(default_value) {}

    // Read-only access — does NOT allocate. Returns default_value if the
    // page backing `key` was never touched.
    [[nodiscard]] auto get(uint32_t key) const -> const T&
    {
        uint32_t p = page_of(key);
        if (p >= pages.size() || !pages[p]) { return default_value; }
        return (*pages[p])[offset_of(key)];
    }

    // Write access — allocates the backing page on first touch.
    [[nodiscard]] auto get_or_create(uint32_t key) -> T&
    {
        uint32_t p = page_of(key);
        if (p >= pages.size()) { pages.resize(p + 1); }
        if (!pages[p]) {
            pages[p] = std::make_unique<Page>();
            pages[p]->fill(default_value);
        }
        return (*pages[p])[offset_of(key)];
    }

    // Explicit set — same allocate-on-write semantics as get_or_create.
    void set(uint32_t key, T value)
    {
        get_or_create(key) = std::move(value);
    }

    // Reset a slot back to default_value. Does not free the page (pages are
    // reused as entity IDs are recycled — freeing/reallocating per-entity
    // would defeat the point).
    void reset(uint32_t key)
    {
        uint32_t p = page_of(key);
        if (p < pages.size() && pages[p]) { (*pages[p])[offset_of(key)] = default_value; }
    }

    // Optional: release fully-unused pages back to the allocator. Call this
    // periodically (e.g. once per N frames) if entity IDs cluster and free
    // up whole page ranges — not required for correctness.
    void compact_page(uint32_t page_index)
    {
        if (page_index < pages.size() && pages[page_index]) {
            bool all_default = true;
            for (auto& v : *pages[page_index]) {
                if (!(v == default_value)) { all_default = false; break; }
            }
            if (all_default) { pages[page_index].reset(); }
        }
    }
};
```

Notes on this design:

- `PageSize = 1024` as you said. `4096 / 1024 = 4` pages to cover today's
  `MAX_ENTITIES`, but the whole point is `MAX_ENTITIES` stops being a hard
  ceiling — `pages` grows as needed (`std::vector`), so you can raise entity
  counts without recompiling constants.
- `compact_page` is optional/manual — real-time reclaiming on every
  `destroy()` would mean scanning the whole page every removal, which is
  wasteful. Leave pages allocated; they get reused as IDs are recycled from
  `free_ids`.
- `Signature` and `uint32_t` both support `operator==`/copy trivially, so
  `T = Signature` and `T = uint32_t` both work with this template as-is.

### Integration points

**`entity_manager.hpp`** — replace the flat signature array:

```cpp
// before
std::array<Signature, MAX_ENTITIES> signatures{};

// after
PagedSparseArray<Signature, 1024> signatures;
```

Every call site (`signatures.at(chosen)`, `.at(e)`) becomes
`.get_or_create(chosen)` for writes and `.get(e)` for reads — `.get()`
returns `const&` so `has_component`/`get_signature` (read-only) use it
directly; `create`/`set_component`/`unset_component`/`set_signature`
(writes) use `.get_or_create()`.

```cpp
[[nodiscard]] auto create(Signature sig) -> Entity
{
    Entity chosen{};
    if (!free_ids.empty()) {
        chosen = free_ids.back();
        free_ids.pop_back();
    } else {
        chosen = next_id++;   // MAX_ENTITIES assert can be dropped or kept as a soft cap
    }
    signatures.get_or_create(chosen) = sig;
    return chosen;
}

void destroy(Entity e)
{
    assert(signatures.get(e).any() && "Entity is not in use");
    signatures.reset(e);
    free_ids.emplace_back(e);
}
```

**`component_manager.hpp`** — replace `entity_to_idx`:

```cpp
// before
std::array<uint32_t, MAX_ENTITIES> entity_to_idx{};

// after
PagedSparseArray<uint32_t, 1024> entity_to_idx{INVALID};
```

`add_data`/`remove_data`/`has_data` swap `entity_to_idx[e]` for
`entity_to_idx.get(e)` (read) or `.get_or_create(e)` (write). Note: this
class is superseded entirely in Part 2 (archetype columns replace
`ComponentArray<T>`), so treat this as an interim step if you want to ship
Part 1 standalone before starting Part 2.

**`system_base.hpp`** — replace `sparse`:

```cpp
// before
std::array<uint32_t, MAX_ENTITIES> sparse{};

// after
PagedSparseArray<uint32_t, 1024> sparse{INVALID};
```

Same substitution pattern in `add_entity`/`remove_entity`/`has_entity`. Note
the move constructor (`SystemBase(SystemBase&&)`) currently does
`sparse(other.sparse)` (array copy) — with `PagedSparseArray` this should
become `sparse(std::move(other.sparse))` since it now owns
`unique_ptr`s and isn't trivially copyable.

This class is also superseded in Part 2 — systems stop owning a sparse set
of entities entirely once archetype tables exist (see below). Still worth
doing now if you want to ship Part 1 independently, since it's a real,
immediate memory win.

### Testing this part in isolation

- Unit test: create 5000 entities (beyond old `MAX_ENTITIES=4096`), destroy
  half, recreate, check signatures/system membership still correct.
- Sanity check page count: with entities clustered at low IDs (typical),
  you should see far fewer pages allocated across all
  `PagedSparseArray` instances than `MAX_ENTITIES / PageSize` would
  suggest if entities were spread out.

---

## Part 2 — Archetype-Based Storage

### What changes conceptually

Today: components live in `ComponentArray<T>` (one dense array per type,
scattered independently), and *each system also keeps its own sparse set*
of qualifying entities (`SystemBase::dense`/`sparse`), updated via
`SystemManagerImpl::on_signature_change` fanning out to every system on
every `add_component`/`remove_component` call.

After: entities with the same component signature live together in one
**archetype table** — one contiguous array (column) per component type,
all columns the same length, row `i` across all columns belongs to the same
entity. A system no longer tracks entities at all — it just asks "which
tables have at least my required components?" and iterates those tables'
columns directly, in lockstep, zero random access.

This also means `archetype.hpp`'s role changes: today `Archetype<CList,
Ts...>` is explicitly a non-storage convenience type (see its own header
comment). After this rewrite it becomes the literal key into the table map
— every entity's storage location *is* determined by its archetype
signature.

### New file: `ecs/column.hpp` — type-erased component storage

An archetype table holds several different `T`s side by side, so we can't
use `tuple<ComponentArray<Ts>...>` per table (that's combinatorial in the
number of distinct signatures actually used). Instead, one type-erased
`Column`, built from a small vtable-free "ops" struct captured per `T` at
compile time.

```cpp
#pragma once

#include "common.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

// ColumnOps
// ==========
// The only per-type "virtual" behavior a Column needs. Built once per T via
// make_column_ops<T>() — function pointers, no vtable, no heap for the ops
// themselves (stored by value in the Column).
struct ColumnOps
{
    void   (*construct_default)(void* dst);
    void   (*move_construct)(void* dst, void* src); // move src -> uninitialized dst
    void   (*destroy)(void* obj);
    size_t element_size;
    size_t element_align;
};

template <ComponentType_t T>
constexpr auto make_column_ops() -> ColumnOps
{
    return ColumnOps{
        .construct_default = [](void* dst) { new (dst) T(); },
        .move_construct     = [](void* dst, void* src) {
            new (dst) T(std::move(*static_cast<T*>(src)));
        },
        .destroy = [](void* obj) { static_cast<T*>(obj)->~T(); },
        .element_size  = sizeof(T),
        .element_align = alignof(T),
    };
}

// Column
// =======
// Type-erased, growable, contiguous storage for one component type within
// one archetype table. Rows are managed by ArchetypeTable (all columns in a
// table stay the same length); Column just knows how to grow/destroy/move
// raw bytes using the ops it was built with.
class Column
{
  private:
    std::byte* buf      = nullptr;
    uint32_t   size_     = 0;
    uint32_t   capacity_ = 0;
    ColumnOps  ops;

    void reserve(uint32_t new_cap)
    {
        if (new_cap <= capacity_) { return; }
        auto* new_buf = static_cast<std::byte*>(
            ::operator new(new_cap * ops.element_size,
                            std::align_val_t(ops.element_align)));

        for (uint32_t i = 0; i < size_; ++i) {
            ops.move_construct(new_buf + i * ops.element_size,
                                buf + i * ops.element_size);
            ops.destroy(buf + i * ops.element_size);
        }
        ::operator delete(buf, std::align_val_t(ops.element_align));
        buf = new_buf;
        capacity_ = new_cap;
    }

  public:
    explicit Column(ColumnOps o) : ops(o) {}

    Column(const Column&) = delete;
    Column(Column&& other) noexcept
        : buf(other.buf), size_(other.size_), capacity_(other.capacity_), ops(other.ops)
    {
        other.buf = nullptr; other.size_ = 0; other.capacity_ = 0;
    }

    ~Column()
    {
        for (uint32_t i = 0; i < size_; ++i) { ops.destroy(buf + i * ops.element_size); }
        if (buf) { ::operator delete(buf, std::align_val_t(ops.element_align)); }
    }

    // Append a default-constructed element, return its row index.
    auto push_default() -> uint32_t
    {
        if (size_ == capacity_) { reserve(capacity_ == 0 ? 64 : capacity_ * 2); }
        ops.construct_default(buf + size_ * ops.element_size);
        return size_++;
    }

    // Swap-remove row `idx` (last element moved into the hole). Caller
    // (ArchetypeTable) is responsible for keeping row_to_entity in sync —
    // this only touches raw storage.
    void swap_remove(uint32_t idx)
    {
        uint32_t last = size_ - 1;
        if (idx != last) {
            ops.destroy(buf + idx * ops.element_size);
            ops.move_construct(buf + idx * ops.element_size, buf + last * ops.element_size);
            ops.destroy(buf + last * ops.element_size);
        } else {
            ops.destroy(buf + idx * ops.element_size);
        }
        --size_;
    }

    // Move the element at `src_row` from `this` into `dst` column's
    // just-pushed-default slot at `dst_row` (used when an entity moves
    // between archetype tables and both tables share this component type).
    void move_into(uint32_t src_row, Column& dst, uint32_t dst_row)
    {
        dst.ops.destroy(dst.buf + dst_row * dst.ops.element_size);
        dst.ops.move_construct(dst.buf + dst_row * dst.ops.element_size,
                                buf + src_row * ops.element_size);
    }

    template <ComponentType_t T>
    [[nodiscard]] auto at(uint32_t row) -> T&
    {
        return *reinterpret_cast<T*>(buf + row * ops.element_size);
    }

    template <ComponentType_t T>
    [[nodiscard]] auto at(uint32_t row) const -> const T&
    {
        return *reinterpret_cast<const T*>(buf + row * ops.element_size);
    }

    [[nodiscard]] auto raw_at(uint32_t row) -> void* { return buf + row * ops.element_size; }
    [[nodiscard]] auto size() const -> uint32_t { return size_; }
};
```

### Global per-component-ID ops table

We need "given a runtime `ComponentType` id, give me a `ColumnOps` to build
a column for it" — resolved once at startup from the compile-time
`ComponentList`.

**New file: `ecs/column_ops_table.hpp`**

```cpp
#pragma once

#include "column.hpp"
#include "component_registry.hpp"
#include <array>

template <typename CList> struct ColumnOpsTable;

template <ComponentType_t... Ts>
struct ColumnOpsTable<ComponentList<Ts...>>
{
    // Indexed by ComponentType id (= position in CList, same as comp_type_index).
    static auto get() -> const std::array<ColumnOps, sizeof...(Ts)>&
    {
        static const std::array<ColumnOps, sizeof...(Ts)> table {
            make_column_ops<Ts>()...
        };
        return table;
    }
};
```

### New file: `ecs/archetype_table.hpp`

```cpp
#pragma once

#include "column.hpp"
#include "column_ops_table.hpp"
#include "common.hpp"
#include <unordered_map>
#include <vector>

// ArchetypeTable
// ===============
// All entities with exactly `sig` live here. One Column per set bit in
// `sig`. row_to_entity[row] and entity are kept 1:1; ArchetypeStore keeps
// the reverse (Entity -> {table, row}) mapping.
class ArchetypeTable
{
  private:
    Signature                              sig;
    std::vector<ComponentType>             comp_ids;      // sorted, matches columns order
    std::unordered_map<ComponentType, uint32_t> col_index; // comp id -> index into columns
    std::vector<Column>                    columns;
    std::vector<Entity>                    row_to_entity;

    // Archetype graph — cached transitions to avoid a signature hash-map
    // lookup on every add_component/remove_component.
    std::array<ArchetypeTable*, MAX_COMPONENTS> add_edge{};
    std::array<ArchetypeTable*, MAX_COMPONENTS> remove_edge{};

  public:
    template <typename CList>
    ArchetypeTable(Signature s, const std::array<ColumnOps, CList::count>& ops_table)
        : sig(s)
    {
        for (ComponentType c = 0; c < MAX_COMPONENTS; ++c) {
            if (sig.test(c)) {
                col_index[c] = static_cast<uint32_t>(columns.size());
                comp_ids.push_back(c);
                columns.emplace_back(ops_table[c]);
            }
        }
    }

    [[nodiscard]] auto signature() const -> Signature { return sig; }
    [[nodiscard]] auto has_component(ComponentType c) const -> bool { return sig.test(c); }
    [[nodiscard]] auto row_count() const -> uint32_t { return static_cast<uint32_t>(row_to_entity.size()); }
    [[nodiscard]] auto entity_at(uint32_t row) const -> Entity { return row_to_entity[row]; }

    template <ComponentType_t T>
    [[nodiscard]] auto column() -> Column&
    {
        return columns[col_index.at(component_id_of<T>())]; // component_id_of<T>: see note below
    }

    // Add a new row (all columns default-constructed), return its row index.
    auto add_row(Entity e) -> uint32_t
    {
        uint32_t row = 0;
        for (auto& col : columns) { row = col.push_default(); }
        row_to_entity.push_back(e);
        return row;
    }

    // Remove row via swap-pop across every column; returns the entity that
    // got moved into `row` (so ArchetypeStore can fix its location table),
    // or INVALID if `row` was the last row.
    auto remove_row(uint32_t row) -> Entity
    {
        uint32_t last = row_count() - 1;
        Entity moved = (row != last) ? row_to_entity[last] : INVALID;
        for (auto& col : columns) { col.swap_remove(row); }
        if (row != last) { row_to_entity[row] = row_to_entity[last]; }
        row_to_entity.pop_back();
        return moved;
    }

    // Move row `src_row` from `*this` into `dst`, for every component type
    // both tables share. Caller adds any *new* component's value into dst
    // separately (see ArchetypeStore::add_component). Returns dst's new row.
    auto move_row_into(uint32_t src_row, ArchetypeTable& dst, Entity e) -> uint32_t
    {
        uint32_t dst_row = dst.row_to_entity.size();
        dst.row_to_entity.push_back(e);
        for (auto c : comp_ids) {
            auto it = dst.col_index.find(c);
            if (it != dst.col_index.end()) {
                // shared component: move existing value across
                columns[col_index[c]].move_into(src_row, dst.columns[it->second], dst_row);
            }
            // else: component was removed in this transition, drop it (column stays behind)
        }
        // Any column present in dst but not in *this is a newly-added
        // component — ArchetypeStore default-constructs it right after this
        // call and then overwrites with the caller's value.
        for (auto& col : dst.columns) {
            if (col.size() <= dst_row) { col.push_default(); }
        }
        return dst_row;
    }

    // Edge cache accessors, used by ArchetypeStore
    auto add_edge_for(ComponentType c) -> ArchetypeTable*& { return add_edge[c]; }
    auto remove_edge_for(ComponentType c) -> ArchetypeTable*& { return remove_edge[c]; }
};
```

**Note on `component_id_of<T>()`**: `Column::at<T>()` needs a runtime
`ComponentType` to index `col_index`, but callers (systems) know `T` at
compile time. Expose this the same way `World` already does:
`World::component_id<T>` (a `static constexpr` already computed from
`comp_type_index<T, MyComponentList>::value`). `ArchetypeTable::column<T>()`
should just take that value as a template parameter resolved the same way
— easiest is to have `World` pass `component_id<T>` down rather than
duplicating the lookup inside `archetype_table.hpp` (which doesn't know
`MyComponentList`). Keep `archetype_table.hpp` free of any dependency on the
game's component list; it only ever deals in `ComponentType` (uint8_t) ids.

### New file: `ecs/archetype_store.hpp`

```cpp
#pragma once

#include "archetype_table.hpp"
#include "column_ops_table.hpp"
#include "paged_array.hpp"
#include <memory>
#include <unordered_map>

struct EntityLoc { ArchetypeTable* table = nullptr; uint32_t row = INVALID; };

template <typename CList>
class ArchetypeStore
{
  private:
    std::unordered_map<Signature, std::unique_ptr<ArchetypeTable>> tables;
    PagedSparseArray<EntityLoc, 1024> locations;
    ArchetypeTable* empty_table;
    uint32_t version_ = 0; // bumped whenever a NEW table is created

    auto get_or_create_table(Signature sig) -> ArchetypeTable*
    {
        auto it = tables.find(sig);
        if (it != tables.end()) { return it->second.get(); }
        auto tbl = std::make_unique<ArchetypeTable>(sig, ColumnOpsTable<CList>::get());
        auto* ptr = tbl.get();
        tables.emplace(sig, std::move(tbl));
        ++version_;
        return ptr;
    }

  public:
    ArchetypeStore() { empty_table = get_or_create_table(Signature{}); }

    [[nodiscard]] auto version() const -> uint32_t { return version_; }

    // Every entity starts here — no components yet.
    auto create_entity(Entity e) -> void
    {
        uint32_t row = empty_table->add_row(e);
        locations.set(e, EntityLoc{empty_table, row});
    }

    void destroy_entity(Entity e)
    {
        EntityLoc loc = locations.get(e);
        Entity moved = loc.table->remove_row(loc.row);
        if (moved != INVALID) { locations.get_or_create(moved).row = loc.row; }
        locations.reset(e);
    }

    template <ComponentType_t T>
    void add_component(Entity e, ComponentType comp_id, T value)
    {
        EntityLoc loc = locations.get(e);
        ArchetypeTable* src = loc.table;

        ArchetypeTable*& edge = src->add_edge_for(comp_id);
        if (!edge) {
            Signature new_sig = src->signature();
            new_sig.set(comp_id);
            edge = get_or_create_table(new_sig);
            edge->add_edge_for(comp_id) = nullptr; // filled lazily on demand elsewhere if needed
        }
        ArchetypeTable* dst = edge;

        uint32_t dst_row = src->move_row_into(loc.row, *dst, e);
        dst->template column<T>().template at<T>(dst_row) = std::move(value);

        Entity moved = src->remove_row(loc.row);
        if (moved != INVALID) { locations.get_or_create(moved).row = loc.row; }

        locations.set(e, EntityLoc{dst, dst_row});
    }

    template <ComponentType_t T>
    void remove_component(Entity e, ComponentType comp_id)
    {
        EntityLoc loc = locations.get(e);
        ArchetypeTable* src = loc.table;

        ArchetypeTable*& edge = src->remove_edge_for(comp_id);
        if (!edge) {
            Signature new_sig = src->signature();
            new_sig.reset(comp_id);
            edge = get_or_create_table(new_sig);
        }
        ArchetypeTable* dst = edge;

        uint32_t dst_row = src->move_row_into(loc.row, *dst, e);
        Entity moved = src->remove_row(loc.row);
        if (moved != INVALID) { locations.get_or_create(moved).row = loc.row; }

        locations.set(e, EntityLoc{dst, dst_row});
    }

    [[nodiscard]] auto get_signature(Entity e) const -> Signature
    {
        return locations.get(e).table->signature();
    }

    template <ComponentType_t T>
    [[nodiscard]] auto get_component(Entity e) -> T&
    {
        EntityLoc loc = locations.get(e);
        return loc.table->template column<T>().template at<T>(loc.row);
    }

    // Collect every table whose signature is a superset of `required`.
    // O(num_tables) — fine, num distinct archetypes is small (tens, not
    // thousands) in practice. Called only when a system's cached version
    // is stale, not every frame.
    void collect_matching(Signature required, std::vector<ArchetypeTable*>& out) const
    {
        out.clear();
        for (auto& [sig, tbl] : tables) {
            if ((sig & required) == required) { out.push_back(tbl.get()); }
        }
    }

    // Direct table creation for World::create_from_archetype — build the
    // exact target table in one step instead of N incremental add_component
    // moves, then fill every column in a single pass.
    auto get_or_create_table_for(Signature sig) -> ArchetypeTable*
    {
        return get_or_create_table(sig);
    }
};
```

This is a **design skeleton**, not drop-in-compile-ready code — a few
things to nail down while implementing (flagged here rather than glossed
over):

- `add_component`'s `move_row_into` + immediate `remove_row(loc.row)`
  sequence works but does one redundant pass; a production version would
  likely merge "move shared columns" and "remove from source" into a single
  function (`ArchetypeTable::transfer_row`) to avoid the double
  bookkeeping shown above. The two-function version above is written for
  clarity, not for being the final call graph.
- `add_edge`/`remove_edge` sizes are `MAX_COMPONENTS` per table — fine
  (32 pointers = 256 bytes/table, and there are few tables).
- `ArchetypeTable::column<T>()` needs `component_id` as a template
  argument resolved via `World::component_id<T>`, not looked up from
  inside `archetype_table.hpp` — keep that file free of any dependency on
  the concrete `ComponentList`.

### Rewriting `system_base.hpp`

Systems drop the `dense`/`sparse` sparse-set entirely. Instead they hold a
cached list of matching tables, refreshed only when the store's `version()`
changes (i.e. only when a *new* archetype signature is created — not on
every entity add/remove).

```cpp
template <typename Derived, typename Store, typename... ComponentTypes>
class SystemBase
{
  protected:
    Store& store;
    static constexpr Signature signature = make_signature<typename Store::ListType, ComponentTypes...>();

    mutable std::vector<ArchetypeTable*> matching_tables;
    mutable uint32_t cached_version = 0xFFFFFFFF;

    void refresh_if_stale() const
    {
        if (cached_version != store.version()) {
            store.collect_matching(signature, matching_tables);
            cached_version = store.version();
        }
    }

    // Iterate every matching table's rows, calling
    // f(Entity, ComponentTypes&...) for each row. Fully sequential within
    // a table — no random access, because every column in a table has the
    // same length and row ordering.
    template <typename F>
    void for_each(F&& f)
    {
        refresh_if_stale();
        for (auto* table : matching_tables) {
            (table->template column<ComponentTypes>(), ...); // materialize refs once per table
            for (uint32_t row = 0; row < table->row_count(); ++row) {
                f(table->entity_at(row), table->template column<ComponentTypes>().template at<ComponentTypes>(row)...);
            }
        }
    }

  public:
    explicit SystemBase(Store& s) : store(s) {}
    void update(float dt) { static_cast<Derived*>(this)->update_impl(dt); }
    [[nodiscard]] static constexpr auto get_signature() -> Signature { return signature; }
};
```

What disappears entirely with this change:

- `SystemBase::dense`/`sparse`, `add_entity`/`remove_entity`/`has_entity`
- `SystemManagerImpl::on_signature_change` and its per-system
  `check_and_update_entity` fan-out — membership is now implicit in which
  table an entity's row lives in, there is nothing to "check" per system
  per component change.
- `SystemManagerImpl::on_entity_destroyed`'s per-system removal loop —
  destruction is one `ArchetypeStore::destroy_entity` call.

`SystemManagerImpl::update()`/`update_system()` stay basically as-is (they
still just fold-call `.update(dt)` over the tuple of systems).

### Rewriting `movement_system.hpp` (worked example)

Before, this system drove iteration off the smaller `RigidBody2` array and
took one random lookup per entity into `Transform2`. After: both
`Transform2` and `RigidBody2` live in the same table (since together they
define the signature the system requires), same row, same order — zero
random access, not "one direct + one random."

```cpp
template <typename Store>
class MovementSystem : public SystemBase<MovementSystem<Store>, Store, Transform2, RigidBody2>
{
  public:
    using Base = SystemBase<MovementSystem<Store>, Store, Transform2, RigidBody2>;
    explicit MovementSystem(Store& s) : Base(s) {}

    void update_impl(float dt)
    {
        this->for_each([dt](Entity /*e*/, Transform2& t, RigidBody2& rb) {
            t.pos.x += rb.v.x * dt;
            t.pos.y += rb.v.y * dt;
        });
    }
};
```

Note this system now only matches entities that have **both**
`Transform2` and `RigidBody2` in the same table — the old comment about
"almost everything has Transform2 but not everything has RigidBody2" is
handled automatically: entities with only `Transform2` simply live in a
different table (`{Transform2}`) that this system's `matching_tables`
won't include.

### Rewriting `world.hpp`

The public API surface barely changes — that's the point of centralizing
storage behind `ArchetypeStore`. Internals route through the store instead
of `EntityManager` signatures + `ComponentManager`:

```cpp
private:
    EntityManager                 entity_manager;   // ID lifecycle only now
    ArchetypeStore<MyComponentList> archetype_store;
    SystemManager                 system_manager {archetype_store};

public:
    [[nodiscard]] auto create_entity() -> Entity
    {
        Entity e = entity_manager.create_bare(); // see note below
        archetype_store.create_entity(e);
        return e;
    }

    void destroy_entity(Entity e)
    {
        archetype_store.destroy_entity(e);
        entity_manager.destroy(e);
    }

    template <ComponentType_t T>
    void add_component(Entity e, T comp)
    {
        archetype_store.template add_component<T>(e, component_id<T>, std::move(comp));
    }

    template <ComponentType_t T>
    void remove_component(Entity e)
    {
        archetype_store.template remove_component<T>(e, component_id<T>);
    }

    template <ComponentType_t T>
    [[nodiscard]] auto get_component(Entity e) -> T&
    {
        return archetype_store.template get_component<T>(e);
    }

    // Archetype creation now builds the target table directly and fills
    // every column in one pass instead of N incremental table moves.
    template <ArchetypeType_t A, typename... Args>
    [[nodiscard]] auto create_from_archetype(Args&&... args) -> Entity
    {
        Entity e = entity_manager.create_bare();
        ArchetypeTable* table = archetype_store.get_or_create_table_for(A::signature());
        uint32_t row = table->add_row(e);
        // fill each column directly instead of moving through the empty table
        (table->template column<std::decay_t<Args>>().template at<std::decay_t<Args>>(row)
             = std::forward<Args>(args), ...);
        archetype_store.set_location(e, table, row); // small accessor to add alongside the others above
        return e;
    }
```

**Intentional API change worth flagging explicitly:** `EntityManager::create(Signature)`
required a non-empty signature up front (`assert(sig.any())`), because the
old model needed to know the signature before any component data existed.
With archetypes, every entity starts in the **empty table** and moves as
components are added — there's no such thing as "create with signature but
no data" anymore, because a table's columns must have real values for
every row. So:

- `EntityManager` loses `signatures`/`set_component`/`unset_component`/
  `has_component`/`get_signature` — that's all owned by `ArchetypeStore`
  now (a table's signature *is* the entity's signature). `EntityManager`
  shrinks down to just ID issuing/recycling (`create_bare()`/`destroy`).
- `World::create_entity(Signature)` becomes `World::create_entity()` —
  no signature parameter, since there's nothing meaningful to pass yet.
  Callers that used to do `create_entity(sig)` then presumably called
  `add_component` for each bit anyway (the original code's
  `create_entity` didn't actually populate component data — it only set
  the signature, which callers had to reconcile themselves); this rewrite
  makes that the *only* path, which is more consistent than what exists
  today, not less.
- `create_from_archetype` is still the fast path for the common case
  (spawn an entity with a known, fixed component set) — it goes straight to
  the target table with one row insert, no incremental moves.

### File-by-file summary of changes

| File | Change |
|---|---|
| `paged_array.hpp` | **new** — Part 1 |
| `column.hpp` | **new** — type-erased column storage |
| `column_ops_table.hpp` | **new** — per-component-id `ColumnOps` lookup |
| `archetype_table.hpp` | **new** — replaces per-type `ComponentArray<T>` |
| `archetype_store.hpp` | **new** — replaces most of `component_manager.hpp` + the signature half of `entity_manager.hpp` |
| `entity_manager.hpp` | Shrinks to ID issue/recycle only; drop `signatures` and all component-bit methods |
| `component_manager.hpp` | Removed (superseded by `archetype_table.hpp` + `archetype_store.hpp`) once Part 2 lands — keep only if you want a transition period where both coexist |
| `component_registry.hpp` | Unchanged — `ComponentList`/`comp_type_index` still needed for `ColumnOpsTable` and `World::component_id` |
| `system_base.hpp` | Drop `dense`/`sparse`; add `matching_tables` cache + `for_each` |
| `system_manager.hpp` | Drop `on_signature_change`/`check_and_update_entity`/the entity-removal fan-out in `on_entity_destroyed` — nothing left to synchronize per-entity |
| `system_registry.hpp` | Unchanged |
| `archetype.hpp` | Meaning changes from "creation convenience, not storage" to "the actual storage key" — update its header comment; `signature()`/`for_each_type` stay the same, they're exactly what `create_from_archetype` needs |
| `world.hpp` | `create_entity()` loses its `Signature` parameter; internals route through `ArchetypeStore`; `create_from_archetype` fills the target table directly |
| `game_registry.hpp` | `MovementSystem<ComponentManager>` → `MovementSystem<ArchetypeStore<MyComponents>>` (systems are now templated on the store type, not the component manager type) |
| `systems/movement_system.hpp` | Rewritten to use `for_each` (shown above) |
| `systems/{input,render,collision}_system.hpp` | Empty stubs today — write them directly against the new `for_each` API, no migration needed |

---

## Suggested implementation order

1. **Part 1 in isolation** — add `paged_array.hpp`, swap it into
   `entity_manager.hpp`, `component_manager.hpp`, `system_base.hpp`. This
   compiles and runs standalone with today's architecture unchanged. Ship
   and test this before touching anything else.
2. **`column.hpp` + `column_ops_table.hpp`** — pure new code, no existing
   file depends on it yet, easy to unit test alone (push/pop/move a few
   `int`s and a non-trivial struct through a `Column`, check ctor/dtor
   counts).
3. **`archetype_table.hpp`** — build and unit test against a hand-written
   `ColumnOps` array (no `ArchetypeStore` yet): create a table, add rows,
   remove rows, check swap-remove correctness on entity/component data.
4. **`archetype_store.hpp`** — wire tables together with the edge cache and
   `PagedSparseArray<EntityLoc>` location table.
5. **`system_base.hpp` + `movement_system.hpp`** — switch to `for_each`,
   confirm `MovementSystem` still produces identical output on a test scene.
6. **`world.hpp` + `game_registry.hpp`** — flip the public API over last,
   once everything underneath it is proven.
7. Delete `component_manager.hpp` and the signature-tracking half of
   `entity_manager.hpp` once nothing references them.

## Open questions to settle during implementation (not blocking the plan)

- Whether `Column` growth should itself be paged (fixed 1024-element
  blocks, like the sparse arrays) instead of geometric `reserve()`/realloc.
  Geometric growth is simpler and the realloc cost is amortized, but paged
  columns would avoid ever moving already-placed component data on growth
  (useful if you ever want stable pointers into a column). Not required for
  the cache-locality win you're after — recommend starting with geometric
  growth and only paging columns if profiling says otherwise.
- Whether to cap the archetype table count / warn on "archetype explosion"
  if the game ends up creating many near-duplicate signatures — not a
  concern at the scale implied by `MAX_COMPONENTS = 32`, but worth a
  comment in `archetype_store.hpp` for future maintainers.
- `move_row_into`'s two-pass shape (move shared columns, then separately
  remove from source) — collapse into a single `transfer_row` once the
  design is implemented and tested, as noted above.
