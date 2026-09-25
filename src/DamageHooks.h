#pragma once

namespace kr {

// Observes the health damage the player deals, straight from the engine.
//
// Two vfunc slots are hooked (slot 0x104, HandleHealthDamage):
//   * RE::VTABLE_Actor[0]
//   * RE::VTABLE_Character[0]  (all non-player actors: NPCs and creatures)
// The player's own vtable is deliberately not hooked: a target that is the player is
// rejected before anything is read, so that slot could never produce a report.
//
// The hook only observes. It chains into the previous entry exactly once, never writes to
// the damage value, the attacker or the target, and reports a single event per real health
// loss through GameBridge::SubmitPlayerDamage. No weapon, hostility, equipment or PO3
// dependency is involved.
//
// Idempotent and main thread only: call once after the data load, on the same thread that
// runs SKSE::MessagingInterface::kDataLoaded.
void InstallDamageHooks();

}
