#pragma once

// game_registry.hpp — THE single file to edit to extend the ECS.
//
// Include order here is the only place that matters. Everything else in the
// library uses comp_type_index<T,List>::value directly and has no include-order
// dependency.
//
// To add a component: add its type to MyComponents (order = permanent ID).
// To add a system:    add its type to MySystems    (order = schedule bit).
//                     Then include its header below.


// 1. Library infrastructure (no game types, no system headers)
#include "common.hpp"
#include "component_manager.hpp"
#include "component_registry.hpp"
#include "system_registry.hpp"
#include "world.hpp"

// 2. Game component headers
#include "2d_comps.hpp"
using namespace ECS_COMPS_2D;


// 3. Define the component list — ORDER IS PERMANENT
using MyComponents = ComponentList<
    Transform2,   // ID 0
    RigidBody2    // ID 1
    // ...
>;


// 4. Convenience alias: component_id<T>
// This is the public API for compile-time component ID lookup.
// Delegates to comp_type_index which is defined in component_registry.hpp.
template <typename T>
constexpr ComponentType component_id = comp_type_index<T, MyComponents>::value;

// Sanity checks — caught at compile time if IDs shift
static_assert(component_id<Transform2> == 0, "Transform2 ID changed — check MyComponents order");
static_assert(component_id<RigidBody2> == 1, "RigidBody2 ID changed — check MyComponents order");

// 5. ComponentManager alias — needed by system headers
// Systems are templated on CMgr (the concrete ComponentManagerImpl type).
// We alias it here so system headers can use it by name.
using ComponentManager = make_component_manager<MyComponents>::type;

// 6. System headers — included AFTER ComponentManager is defined
// Systems are templated on CMgr so they don't need to be included before
// the lists are defined. The template parameter carries the list via CMgr::ListType.
#include "movement_system.hpp"
// Add new system headers here.


// 7. Define the system list — ORDER IS PERMANENT (= schedule bit position)
using MySystems = SystemList<
    MovementSystem<ComponentManager>   // bit 0
    // ...
>;


// 8. Convenience alias: system_id<T>
template <typename T>
constexpr SystemType system_id = sys_type_index<T, MySystems>::value;

// Sanity checks
static_assert(system_id<MovementSystem<ComponentManager>> == 0, "MovementSystem ID changed");

// 9. Instantiate World
using GameWorld = World<MyComponents, MySystems>;

// 10. Named schedules — constexpr, computed once, embedded as literals
constexpr Schedule MOVEMENT_ONLY =
    GameWorld::make_schedule<MovementSystem<ComponentManager>>();

constexpr Schedule ALL_SYSTEMS =
    GameWorld::make_schedule<
        MovementSystem<ComponentManager>
    >();


// 11. Named archetypes
using PlayerArchetype = GameWorld::Archetype<Transform2, RigidBody2>;



