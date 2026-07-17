#pragma once

// ComponentArray<T>            — dense/sparse storage for one component type
// ComponentManagerImpl<Ts...>  — tuple of all arrays, no heap, no vtable
// make_component_manager<List> — unpacks a ComponentList into ComponentManagerImpl

#include "common.hpp"
#include "component_registry.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <tuple>
#include <vector>


// ComponentArray<T>
// ==================
// Dense/sparse storage for a single component type T.
//
// Layout:
//   data[i]           — component value for the i-th active entity
//   idx_to_entity[i]  — which entity owns slot i  (for swap-and-pop)
//   entity_to_idx[e]  — which slot entity e uses  (INVALID = not present)
//
// All operations are O(1). Removal uses swap-and-pop to keep data dense.

template <ComponentType_t T>
class ComponentArray
{
  private:
    std::vector<T>                     data;
    std::vector<Entity>                idx_to_entity;
    std::array<uint32_t, MAX_ENTITIES> entity_to_idx{};

  public:

    ComponentArray() {
        data.reserve(PRE_INIT_SIZE);
        idx_to_entity.reserve(PRE_INIT_SIZE);
        entity_to_idx.fill(INVALID);
    }

    // Takes T by value — caller can pass rvalue or lvalue, we always move in
    void add_data(Entity e, T comp)
    {
        assert(e < MAX_ENTITIES       && "Entity out of range");
        assert(entity_to_idx[e] == INVALID && "Entity already has component");

        entity_to_idx[e] = static_cast<uint32_t>(data.size());
        idx_to_entity.emplace_back(e);
        data.emplace_back(std::move(comp));
    }

    void remove_data(Entity e)
    {
        assert(e < MAX_ENTITIES       && "Entity out of range");
        assert(entity_to_idx[e] != INVALID && "Entity does not have component");

        uint32_t    idx         = entity_to_idx[e];
        uint32_t    last_idx    = static_cast<uint32_t>(data.size()) - 1;
        Entity last_entity = idx_to_entity[last_idx];

        // When you remove the last entity in a ComponentArray, idx == last_idx.
        // The swap-and-pop then reads from a slot that's already logically removed,
        // and entity_to_idx[last_entity] = idx writes back a now-invalid index
        // before pop_back clears it. Needs a guard:
        if (idx != last_idx) {
            // Move last element into the vacated slot
            data[idx]               = std::move(data[last_idx]);
            idx_to_entity[idx]      = last_entity;
            entity_to_idx[last_entity] = idx;
        }

        // Erase the now-duplicate last slot
        entity_to_idx[e] = INVALID;
        data.pop_back();
        idx_to_entity.pop_back();
    }

    // Safe removal used by entity_destroyed fold
    // raw remove has a runtime assert and direct array access
    void remove_if_present(Entity e)
    {
        if (has_data(e)) { remove_data(e); }
    }

    [[nodiscard]] auto get_data(Entity e) -> T&
    {
        assert(e < MAX_ENTITIES && "Entity out of range");
        assert(entity_to_idx[e] != INVALID && "Entity does not have component");
        return data[entity_to_idx[e]];
    }

    [[nodiscard]] auto get_data(Entity e) const -> const T&
    {
        assert(e < MAX_ENTITIES && "Entity out of range");
        assert(entity_to_idx[e] != INVALID && "Entity does not have component");
        return data[entity_to_idx[e]];
    }

    [[nodiscard]] auto has_data(Entity e) const -> bool
    {
        assert(e < MAX_ENTITIES && "Entity out of range");
        return entity_to_idx[e] != INVALID;
    }

    // Direct sequential access — used by systems to drive iteration from the
    // smallest component array instead of going through the entity set
    [[nodiscard]] auto entity_count() const -> uint32_t
    {
        return static_cast<uint32_t>(data.size());
    }

    [[nodiscard]] auto entity_at(uint32_t idx) const -> Entity
    {
        assert(idx < data.size() && "Index out of range");
        return idx_to_entity[idx];
    }

    [[nodiscard]] auto data_at(uint32_t idx) -> T&
    {
        assert(idx < data.size() && "Index out of range");
        return data[idx];
    }

    [[nodiscard]] auto data_at(uint32_t idx) const -> const T&
    {
        assert(idx < data.size() && "Index out of range");
        return data[idx];
    }
};


// ComponentManagerImpl<Ts...>
// ============================
// Stores one ComponentArray<T> per component type, inline in a tuple.
// No heap allocation, no pointer indirection, no virtual dispatch.
//
// ListType is exported so SystemBase can derive the component list from
// the manager type without needing a separate template parameter.

template <ComponentType_t... Ts>
class ComponentManagerImpl
{
  public:
    // The component list type this manager was instantiated from.
    // Used by SystemBase to call comp_type_index<T, ListType>::value
    // without needing a separate CList template parameter.
    using ListType = ComponentList<Ts...>;

  private:
    // Inline storage: [ ComponentArray<T1> | ComponentArray<T2> | ... ]
    // std::get<ComponentArray<T>>(arrays) resolves at compile time — zero overhead
    std::tuple<ComponentArray<Ts>...> arrays;

  public:

    template <ComponentType_t T>
    auto get_arr() -> ComponentArray<T>&
    {
        return std::get<ComponentArray<T>>(arrays);
    }

    template <ComponentType_t T>
    auto get_arr() const -> const ComponentArray<T>&
    {
        return std::get<ComponentArray<T>>(arrays);
    }

    // Called when an entity is destroyed.
    // Fold expression calls remove_if_present on every array — no signature
    // check needed because remove_if_present is already guarded by has_data().
    // All calls are direct and inlined — no vtable, no loop over a pointer array.
    void entity_destroyed(Entity e)
    {
        std::apply([&](auto&... arr) -> void {
            (arr.remove_if_present(e), ...);
        }, arrays);
    }
};


// Unpacking ComponentList into ComponentManagerImpl
// ==================================================
// Usage: using ComponentManager = make_component_manager<MyComponents>::type;

template <typename List>
struct make_component_manager;

template <ComponentType_t... Ts>
struct make_component_manager<ComponentList<Ts...>> {
    using type = ComponentManagerImpl<Ts...>;
};



