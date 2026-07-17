#pragma once

// Archetype<CList, Ts...>
//
// A named bundle of component types used for ergonomic entity creation.
// NOTE: NOT a storage strategy — components are still stored per-type in
// ComponentArray<T>, not grouped by archetype.
//
// CList is the ComponentList the archetype is defined against. It must be
// passed explicitly because there is no global ComponentList alias — the list
// lives in the user's game_registry.hpp.
//
// Usage:
//   using PlayerArchetype = Archetype<MyComponents, Transform2, RigidBody2, Sprite>;
//   Entity e = world.create_from_archetype<PlayerArchetype>();

#include "common.hpp"
#include "component_registry.hpp"   // comp_type_index
#include <concepts>
#include <cstdint>


// concept for Archetype validation
// =================================

template <typename T>
concept ArchetypeType_t = requires {
    { T::size()      } -> std::same_as<uint8_t>;
    { T::signature() } -> std::same_as<Signature>;
};


// Archetype<CList, Ts...>
// ========================

template <typename CList, typename... Ts>
struct Archetype
{
    // Compute the signature from component IDs at compile time.
    // comp_type_index<T, CList>::value is a constexpr uint8_t — no runtime cost.
    static consteval auto signature() -> Signature
    {
        Signature sig{};
        (sig.set(comp_type_index<Ts, CList>::value), ...);
        return sig;
    }

    // Number of component types in this archetype
    static constexpr auto size() -> uint8_t { return sizeof...(Ts); }

    // Iterate over all component types in this archetype at compile time.
    // Calls f.template operator()<T>() for each T in Ts...
    // Used by World::create_from_archetype to call add_data for each type.
    template <typename F>
    static constexpr void for_each_type(F f)
    {
        (f.template operator()<Ts>(), ...);
    }

    // Check if a type is in this archetype — constexpr fold
    template <typename T>
    static constexpr auto contains() -> bool
    {
        return (std::is_same_v<T, Ts> || ...);
    }
};
