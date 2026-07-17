#pragma once

// SystemBase<Derived, CMgr, ComponentTypes...>
//
// CRTP base for all systems. No virtual functions, no vtable, no ISystem.
//
// Template parameters:
//   Derived          — the concrete system class (CRTP)
//   CMgr             — the concrete ComponentManagerImpl instantiation.
//                      Carries ListType so we can call comp_type_index without
//                      a separate CList parameter.
//   ComponentTypes...— the components this system requires (used to compute
//                      the system's signature at compile time)
//
// Why template on CMgr instead of the ComponentList directly?
//   SystemBase needs both: the manager (to store a ref and call get_arr<T>())
//   and the list (to compute the signature via comp_type_index).
//   CMgr::ListType gives us the list for free — one template param, not two.
//
// make_signature<CList, Ts...>() is a free function that takes the list
// explicitly, using comp_type_index<T, CList>::value

#include "common.hpp"
#include "component_registry.hpp"

#include <array>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <utility>
#include <vector>


// compute a Signature bitset at compile time
// ===========================================
// Called inside SystemBase with CMgr::ListType.

template <typename CList, typename... Ts>
consteval auto make_signature() -> Signature
{
    Signature sig{};
    // For each component type Ts, set the bit at its position in CList
    ((sig.set(comp_type_index<Ts, CList>::value)), ...);
    return sig;
}


// SystemBase
// ===========

template <typename Derived, typename CMgr, typename... ComponentTypes>
class SystemBase
{
  protected:
    // Sparse set — O(1) add, O(1) remove, O(n) sequential iteration
    // dense[i]   = the i-th active entity (contiguous, iteration is sequential)
    // sparse[e]  = index of entity e in dense (INVALID = not in this system)
    //
    // Invariant: sparse[dense[i]] == i for all i < dense.size()
    //            dense[sparse[e]] == e for all e with sparse[e] != INVALID
    std::vector<Entity>           dense;
    std::array<uint32_t, MAX_ENTITIES> sparse{};

    // Single runtime reference — the only non-constexpr data besides the entity set
    CMgr& comp_manager;

    // Signature computed from ComponentTypes at compile time via CMgr::ListType.
    // make_signature resolves comp_type_index<T, ListType>::value for each T.
    static constexpr Signature signature =
        make_signature<typename CMgr::ListType, ComponentTypes...>();

  public:
    auto operator=(const SystemBase&) -> SystemBase& = delete;
    auto operator=(SystemBase&&)      -> SystemBase& = delete;

    explicit SystemBase(CMgr& cm) : comp_manager(cm)
    {
        dense.reserve(PRE_INIT_SIZE);
        sparse.fill(INVALID);
    }

    // Move constructor — needed because SystemManagerImpl stores systems in a
    // tuple and constructs them with Systems(cm)... which may invoke moves
    SystemBase(SystemBase&& other) noexcept
        : comp_manager(other.comp_manager)
        , dense(std::move(other.dense))
        , sparse(other.sparse)
    {}

    // Copy intentionally deleted — systems own their entity sets
    SystemBase(const SystemBase&) = delete;

    ~SystemBase() = default;


    // Entity management

    void add_entity(Entity e)
    {
        assert(e < MAX_ENTITIES  && "Entity out of range");
        assert(!has_entity(e)    && "Entity already in system");

        sparse[e] = static_cast<uint32_t>(dense.size());
        dense.emplace_back(e);
    }

    void remove_entity(Entity e)
    {
        assert(e < MAX_ENTITIES && "Entity out of range");
        assert(has_entity(e)    && "Entity not in system");

        uint32_t    idx         = sparse[e];
        uint32_t    last_idx    = static_cast<uint32_t>(dense.size()) - 1;
        Entity last_entity = dense[last_idx];

        // Only swap if the target isn't already the last element.
        // When idx == last_idx, last_entity == e: writing sparse[e] = idx
        // before clearing it would survive the sparse[e] = INVALID below
        // only because they're the same write — but the dense/sparse state
        // would be consistent regardless. The guard is still correct and
        // makes the intent explicit.
        if (idx != last_idx) {
            dense[idx]          = last_entity;
            sparse[last_entity] = idx;
        }
        dense.pop_back();

        // Clear the removed entity's sparse slot
        sparse[e] = INVALID;
    }

    [[nodiscard]] auto has_entity(Entity e) const -> bool
    {
        assert(e < MAX_ENTITIES && "Entity out of range");
        // Triple check: slot not INVALID, index in bounds, cross-reference valid
        // Guards against stale indices after swap-and-pop
        return sparse[e] != INVALID
            && sparse[e] < static_cast<uint32_t>(dense.size())
            && dense[sparse[e]] == e;
    }

    [[nodiscard]] static constexpr auto get_signature() -> Signature
    {
        return signature;
    }

    [[nodiscard]] auto get_entity_count() const -> uint32_t
    {
        return static_cast<uint32_t>(dense.size());
    }

    // CRTP dispatch — SystemManagerImpl calls sys.update(dt) which calls
    // Derived::update_impl(dt) directly. No virtual dispatch.
    void update(float dt)
    {
        // Compile time Dispatch: That's why we take the Derived class as a parameter
        // now we can cast to the correct Derived type and call it's method, resolved at
        // compile type. In normal oop, this would be virtual in base and require vtable lookup
        static_cast<Derived*>(this)->update_impl(dt);
    }

  protected:
    // Component access helpers
    // get_component<T>(e) goes: comp_manager.get_arr<T>().get_data(e)
    // This is two direct memory accesses — get_arr<T>() is a compile-time
    // std::get<> on the tuple, get_data(e) indexes into the dense vector.

    template <typename T>
    [[nodiscard]] auto get_component(Entity e) -> T&
    {
        return comp_manager.template get_arr<T>().get_data(e);
    }

    template <typename T>
    [[nodiscard]] auto get_component(Entity e) const -> const T&
    {
        return comp_manager.template get_arr<T>().get_data(e);
    }

    template <typename T>
    [[nodiscard]] auto has_component(Entity e) const -> bool
    {
        return comp_manager.template get_arr<T>().has_data(e);
    }
};


// SystemType_t
// =============
// The single, authoritative concept for valid ECS systems.
//
// The inheritance check uses an immediately-invoked generic lambda: the
// compiler must be able to convert T* to SystemBase<D,C,Cs...>* for some
// deduced D, C, Cs... — i.e. T must publicly inherit from some instantiation
// of SystemBase. This rules out duck-typed imposters that happen to expose the
// right method names without going through the CRTP base.
//
// update_impl is the one method SystemBase does NOT provide — it is the pure
// virtual equivalent in CRTP, and each concrete system must define it.
template <typename T>
concept SystemType_t =
    requires(T& sys, float dt) { { sys.update_impl(dt) } -> std::same_as<void>; }
    &&
    requires {
        []<typename D, typename C, typename... Cs>(SystemBase<D, C, Cs...>*) -> auto
            {}(static_cast<T*>(nullptr));
    };

