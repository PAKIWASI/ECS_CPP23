#pragma once

// World<CList, SList>
//
// The public API surface — orchestrates EntityManager, ComponentManager,
// SystemManager, and Archetype creation.
//
// Templated on:
//   CList — ComponentList<Ts...> from game_registry.hpp
//   SList — SystemList<Ts...>    from game_registry.hpp
//
// All type aliases (ComponentManager, SystemManager, component_id, system_id)
// are members of World so they are scoped to the correct lists. This means
// two World instantiations in the same program never share or collide aliases.
//
// make_schedule<Ts...>() is a static member that builds a Schedule bitset
// from system types — it must be a member because it needs SList.
//
// Usage:
//   #include "game_registry.hpp"   // defines MyComponents, MySystems, GameWorld
//   GameWorld world;
//   Entity e = world.create_entity();
//   world.add_component<Transform2>(e, Transform2{});
//   world.update(dt, GameWorld::make_schedule<GravitySystem, MovementSystem>());

#include "common.hpp"
#include "component_manager.hpp"
#include "component_registry.hpp"
#include "entity_manager.hpp"
#include "system_manager.hpp"
#include "system_registry.hpp"
#include "archetype.hpp"
#include <cassert>
#include <cstdint>


// World
// ======
// primary template (intentionally incomplete)
// Forces users to always instantiate with explicit ComponentList and SystemList.
// Catching wrong usage at the template level rather than inside the body.

template <typename CList, typename SList>
class World;


// World<ComponentList<CList...>, SystemList<SList...>>
// =====================================================

template <typename... CList, typename... SList>
class World<ComponentList<CList...>, SystemList<SList...>>
{
  public:

    // Type aliases — scoped to this World instantiation

    using MyComponentList = ComponentList<CList...>;
    using MySystemList    = SystemList<SList...>;

    using ComponentManager = typename make_component_manager<MyComponentList>::type;
    using SystemManager    = typename make_system_manager<MySystemList>::type;

    // Compile-time ID lookup — same interface as the old global aliases but
    // scoped so two World instantiations never collide
    template <typename T>
    static constexpr ComponentType component_id =
        comp_type_index<T, MyComponentList>::value;

    template <typename T>
    static constexpr SystemType system_id =
        sys_type_index<T, MySystemList>::value;

    // Build a Schedule bitset from a list of system types.
    // consteval = computed at compile time, embeds as a bitset literal.
    // Usage: constexpr auto sched = GameWorld::make_schedule<GravitySystem, MovementSystem>();
    template <typename... Ts>
    static consteval auto make_schedule() -> Schedule
    {
        Schedule s{};
        (s.set(sys_type_index<Ts, MySystemList>::value), ...);
        return s;
    }

    // Archetype helper — binds CList so callers don't need to pass it
    template <typename... Ts>
    using Archetype = ::Archetype<MyComponentList, Ts...>;

  private:

    // Stored in construction order — component_manager must be initialized
    // before system_manager because systems hold a ref to it
    EntityManager    entity_manager;
    ComponentManager component_manager {};
    SystemManager    system_manager {component_manager};


  public:

    // Entity API

    [[nodiscard]] auto create_entity(Signature sig) -> Entity
    {
        assert(sig.any() && "Entity should have atleast one Component");
        return entity_manager.create(sig);
    }

    void destroy_entity(Entity e)
    {
        // Order matters: systems and components clean up before the ID is recycled
        system_manager.on_entity_destroyed(e);
        component_manager.entity_destroyed(e);
        entity_manager.destroy(e);
    }


    // TODO: understand
    // Archetype creation
 
    // Overload 1: default-construct each component
    template <ArchetypeType_t A>
    [[nodiscard]] auto create_from_archetype() -> Entity
    {
        // entity_manager.create() now requires a non-empty signature up front
        // (an entity with zero components is meaningless). The archetype's
        // signature is known at compile time, so pass it directly instead of
        // creating bare and patching bits in afterward.
        Entity e = entity_manager.create(A::signature());
        A::for_each_type([&]<typename T>() -> void {
            component_manager.template get_arr<T>().add_data(e, T{});
        });
        system_manager.on_signature_change(e, entity_manager.get_signature(e));
        return e;
    }
 
    // Overload 2: caller supplies initial component values
    // Args must match the archetype's component types in order.
    template <ArchetypeType_t A, typename... Args>
    [[nodiscard]] auto create_from_archetype(Args&&... args) -> Entity
    {
        // Same reasoning as overload 1: pass the full archetype signature to
        // create() up front rather than creating bare and setting bits after.
        Entity e = entity_manager.create(A::signature());
        (
            [&]<typename T>(T&& comp) -> void {
                component_manager.template get_arr<T>().add_data(e, std::forward<T>(comp));
            }(std::forward<Args>(args)),
        ...);
        system_manager.on_signature_change(e, entity_manager.get_signature(e));
        return e;
    }


    // Component API

    template <ComponentType_t T>
    void add_component(Entity e, T comp)
    {
        component_manager.template get_arr<T>().add_data(e, std::move(comp));
        entity_manager.set_component(e, component_id<T>);
        system_manager.on_signature_change(e, entity_manager.get_signature(e));
    }

    template <ComponentType_t T>
    void remove_component(Entity e)
    {
        component_manager.template get_arr<T>().remove_data(e);
        entity_manager.unset_component(e, component_id<T>);
        system_manager.on_signature_change(e, entity_manager.get_signature(e));
    }

    template <ComponentType_t T>
    [[nodiscard]] auto get_component(Entity e) -> T&
    {
        return component_manager.template get_arr<T>().get_data(e);
    }

    template <ComponentType_t T>
    [[nodiscard]] auto get_component(Entity e) const -> const T&
    {
        return component_manager.template get_arr<T>().get_data(e);
    }

    // Expose the manager for systems that hold a direct ref (see SystemBase)
    [[nodiscard]] auto get_component_manager() -> ComponentManager&
    {
        return component_manager;
    }


    // System API

    void update(float dt, Schedule schedule)
    {
        system_manager.update(dt, schedule);
    }

    template <typename T>
    void update_system(float dt)
    {
        system_manager.template update_system<T>(dt);
    }

    template <typename T>
    [[nodiscard]] auto get_entity_count() const -> uint32_t
    {
        return system_manager.template get_entity_count<T>();
    }
};
