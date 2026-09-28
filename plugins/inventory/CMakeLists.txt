# rx::inventory: optional, batteries-included ECS item/inventory/equipment
# system: an item catalog, stacking inventories, generic equipment slots and
# their persistence. Physics-free by design, so a dedicated server or a game
# with no world drops links it without dragging Jolt in.
rx_add_module(inventory
  item_catalog.cc
  inventory.cc
  serialize.cc
)
target_link_libraries(rx_inventory PUBLIC rx::foundation rx::ecs rx::scene rx::asset)

# World-item half: dropping items into the 3D world as physics bodies, the
# hibernation store, and world-item persistence. Split into its own target (the
# same rx::nav / rx::nav_viz split) so the physics dependency stays optional;
# built whenever the physics module exists (it always does - real Jolt or stub).
if(TARGET rx_physics)
  rx_add_module(inventory_world
    world_item.cc
  )
  target_link_libraries(rx_inventory_world PUBLIC rx::inventory rx::physics rx::scene)
endif()

if(RX_BUILD_TESTS AND RX_JOLT)
  # rx::inventory: stacking/capacity/weight, transfer, equip/unequip, then the
  # world-item lifecycle on real Jolt (drop -> settle -> pick up / hibernate /
  # wake) and save/load roundtrips for inventories AND world items. Headless.
  add_executable(inventory_test inventory_test.cc)
  target_link_libraries(inventory_test PRIVATE
    rx::inventory rx::inventory_world rx::physics rx::scene rx::ecs rx::foundation)
  add_test(NAME inventory_test COMMAND inventory_test)
endif()
