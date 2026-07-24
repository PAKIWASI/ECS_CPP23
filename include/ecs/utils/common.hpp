#pragma once

#include <bitset>
#include <concepts>
#include <cstdint>
#include <limits>


// Configuration
// ==============

constexpr uint32_t MAX_ENTITIES  = 4096;
constexpr uint8_t  MAX_COMPONENTS = 32;
constexpr uint8_t  MAX_SYSTEMS    = 16;
// TODO: make this container wise
constexpr uint32_t PRE_INIT_SIZE  = 100;


// Core type aliases
// ==================

// an entity is just and id - an index into each component array, which hold data
using Entity        = uint32_t;
// a bitfield where each bit represents the presence/absense of each component 
using Signature     = std::bitset<MAX_COMPONENTS>;
// the component id type, index into Signature
using ComponentType = uint8_t;
// the system id type, index into Schedule
using SystemType    = uint8_t;
// bits in Schedule represent which systems to run in an update function
// we don't always blindly update each system each frame
using Schedule      = std::bitset<MAX_SYSTEMS>;
// TODO: we pass schedule by value to the update function, it is fune for now with MAX_SYSTEMS=16
// what if it's bigger than 64bits? should we make it future proof by passing by value?
// does it have any overhead

// a sentient representing an entity id that not been issued
// used in dense/sparse arrays where we index into sparse to find actual entity index in dense
static constexpr uint32_t INVALID = std::numeric_limits<uint32_t>::max();


// Component Concept
// ==================

// What counts as a component: must be default-constructible and movable.
// Default-constructible so ComponentArray can pre-allocate slots.
// Movable so we can move data into the array without copying.
template <typename T>
concept ComponentType_t = std::default_initializable<T>
                       && std::movable<T>;


