#pragma once

// MovementSystem
//
// Integrates velocity into position. Generic on purpose — it does not care
// why an entity has velocity, only that it does. GravitySystem, InputSystem,
// CollisionSystem (response/bounce) etc. can all write into RigidBody2.v
// independently as long as they run earlier in the schedule than this system.

#include "common.hpp"
#include "system_base.hpp"
#include "2d_comps.hpp"
#include <cstdint>

using namespace ECS_COMPS_2D;

template <typename CMgr>
class MovementSystem : public SystemBase<MovementSystem<CMgr>, CMgr, Transform2, RigidBody2>
{
  public:
    using Base = SystemBase<MovementSystem<CMgr>, CMgr, Transform2, RigidBody2>;

    explicit MovementSystem(CMgr& cm) : Base(cm) {}

    // Drive loop from RigidBody2 — almost everything has a Transform2 but
    // not everything with a Transform2 has a RigidBody2 (static scenery, UI).
    // Driving from the smaller set avoids processing entities that don't move.
    void update_impl(float dt)
    {
        auto& rigidbodies = this->comp_manager.template get_arr<RigidBody2>();
        auto& transforms  = this->comp_manager.template get_arr<Transform2>();

        for (uint32_t i = 0; i < rigidbodies.entity_count(); ++i)
        {
            Entity e  = rigidbodies.entity_at(i);  // sequential — free
            auto&  rb = rigidbodies.data_at(i);    // sequential — free
            auto&  t  = transforms.get_data(e);    // one random lookup

            t.pos.x += rb.v.x * dt;
            t.pos.y += rb.v.y * dt;
        }
    }
};
