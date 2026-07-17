#pragma once

#include "system_base.hpp"
#include <cstdint>
#include <limits>


// Ordered type list, position = permanent system ID
// ==================================================

template <SystemType_t... Ts>
struct SystemList {
    static constexpr uint8_t count = sizeof...(Ts);
};


// Compile-time system ID lookup
// ==============================
// "What is the ID of type T in SystemList List?"
// Same pattern as comp_type_index in component_registry.hpp.

template <typename T, typename List>
struct sys_type_index;

template <typename T, typename... Ts>
struct sys_type_index<T, SystemList<Ts...>>
{
    static constexpr uint8_t value = []() consteval -> uint8_t
    {
        constexpr bool matches[] { std::is_same_v<T, Ts>... };

        for (uint8_t i = 0; i < sizeof...(Ts); ++i) {
            if (matches[i]) { return i; }
        }

        // T not in list — caught by static_asserts in game_registry.hpp
        return std::numeric_limits<uint8_t>::max();
    }();
};

