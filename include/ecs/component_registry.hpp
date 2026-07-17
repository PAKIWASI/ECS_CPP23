#pragma once

// ComponentList and comp_type_index — pure library infrastructure
// Game components are coupled to the user's game_registry.hpp

#include "common.hpp"


// ComponentList
// ==============
// ordered type list, position = permanent component ID

// Wraps a parameter pack of component types.
// The index of each type in the pack is its component ID.
template <ComponentType_t... Ts>
struct ComponentList {
    static constexpr uint8_t count = sizeof...(Ts);
};


// Compile-time ID lookup
// =======================
// "What is the ID of type T in ComponentList List?"
// Returns a constexpr uint8_t. Hard error at compile time if T is not in List.

// Primary template — intentionally undefined. Instantiating with anything
// other than a ComponentList triggers an incomplete type error.
template <typename T, typename List>
struct comp_type_index;

// Partial specialization: fires when List = ComponentList<Ts...>
// Unpacks the list and searches for T using a consteval lambda.
template <typename T, typename... Ts>
struct comp_type_index<T, ComponentList<Ts...>>
{
    static constexpr uint8_t value = []() consteval -> uint8_t
    {
        // Expand the pack into a bool array: matches[i] = (T == Ts[i])
        constexpr bool matches[] { std::is_same_v<T, Ts>... };

        for (uint8_t i = 0; i < sizeof...(Ts); ++i) {
            if (matches[i]) { return i; }
        }

        // T is not in this ComponentList.
        // The returned max value will be caught by static_asserts in game_registry.hpp.
        // TODO: We cannot static_assert(false) here because that fires before instantiation.
        return std::numeric_limits<uint8_t>::max();
    }();
};



