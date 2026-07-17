#pragma once

// SystemManagerImpl<Systems...>
//
// Stores all systems directly in a std::tuple — no heap, no pointer indirection,
// no virtual dispatch. All calls go through fold expressions over the tuple.
//
// system_id<T> has no global alias here. Instead, update() uses
// sys_type_index<T, MySysList>::value directly where MySysList is the
// SystemList<Systems...> reconstructed from the template pack.
//
// make_system_manager<SList> unpacks a SystemList into SystemManagerImpl.

#include "common.hpp"
#include "system_base.hpp"
#include "system_registry.hpp"
#include <cstdint>
#include <tuple>
#include <type_traits>


template <SystemType_t... Systems>
class SystemManagerImpl
{
  public:
    // Reconstruct the SystemList type from our pack — used to look up system
    // IDs via sys_type_index<T, MySysList>::value in update()
    using MySysList = SystemList<Systems...>;

  private:
    // Inline storage: [ System0 | System1 | ... ]
    // std::get<T>(systems) resolves at compile time
    std::tuple<Systems...> systems;

  public:

    // Each system's constructor takes CMgr& — forward the same reference to all
    template <typename CMgr>
    explicit SystemManagerImpl(CMgr& cm)
        : systems(Systems(cm)...)
    {}

    // Compile-time system lookup
    template <typename T>
    [[nodiscard]] auto get_system() -> T&
    {
        return std::get<T>(systems);
    }

    template <typename T>
    [[nodiscard]] auto get_system() const -> const T&
    {
        return std::get<T>(systems);
    }

    // Signature-based entity routing

    // Called whenever an entity's component set changes (add/remove component).
    // Checks every system: if the entity now qualifies → add it; if it no longer
    // qualifies and was in the system → remove it.
    void on_signature_change(Entity e, Signature new_sig)
    {
        std::apply([&](auto&... sys) -> void {
            (check_and_update_entity(sys, e, new_sig), ...);
        }, systems);
    }

    // Called when an entity is destroyed. Remove it from every system it's in.
    // has_entity() is O(1) on the sparse set — this is safe to call on all systems.
    void on_entity_destroyed(Entity e)
    {
        std::apply([&](auto&... sys) -> void {
            ((sys.has_entity(e) ? sys.remove_entity(e) : void()), ...);
        }, systems);
    }

    // Update
    // Iterates the tuple via fold expression. For each system, checks its bit
    // in the schedule bitset. If set, calls update(dt) directly — no vtable.
    //
    // sys_type_index<T, MySysList>::value is a compile-time constant giving
    // the bit position for system T. remove_cvref_t<decltype(sys)> extracts T
    // from the auto& pack parameter.

    void update(float dt, Schedule schedule)
    {
        std::apply([&](auto&... sys) -> auto {
            ((schedule.test(
                sys_type_index<
                    std::remove_cvref_t<decltype(sys)>,
                    MySysList
                >::value
            ) ? sys.update(dt) : void()), ...);
        }, systems);
    }

    // Direct single-system update — bypasses schedule, useful for fixed-step systems
    template <typename T>
    void update_system(float dt)
    {
        std::get<T>(systems).update_impl(dt);
    }

    template <typename T>
    [[nodiscard]] auto get_entity_count() const -> uint32_t
    {
        return std::get<T>(systems).get_entity_count();
    }

  private:

    // Check whether entity e qualifies for sys and add/remove accordingly.
    // qualifies = system's required bits are all set in new_sig
    template <typename System>
    void check_and_update_entity(System& sys, Entity e, Signature new_sig)
    {
        bool qualifies = (new_sig & System::get_signature()) == System::get_signature();
        bool has       = sys.has_entity(e);

        if      ( qualifies && !has) { sys.add_entity(e); }
        else if (!qualifies &&  has) { sys.remove_entity(e); }
    }
};


// unpacking SystemList into SystemManagerImpl
// ============================================

template <typename List>
struct make_system_manager;

template <SystemType_t... Ts>
struct make_system_manager<SystemList<Ts...>> {
    using type = SystemManagerImpl<Ts...>;
};


