# KratosRagePatch implementation contracts

Runtime plugin: KratosRagePatch.esp (ESL-compatible, form version 44), DLL KratosRage.dll version 0.3.0, runtime 1.6.640 only. The DLL is the only 0.3.0 runtime asset; ESP/SEQ, PEX, SWF, OAR/PI configs and `config.json` are byte-for-byte reused from 0.2.0.

## Local new record IDs
- 0x800 QUST KR_MainQuest: start-game enabled; script KR_Config; forced player reference alias ID 0 with KR_PlayerAlias.
- 0x801 QUST KR_WidgetQuest: start-game enabled; script KR_Widget.
- 0x802 SPEL KR_RageReadyAbility: constant self ability using 0x803 only.
- 0x803 MGEF KR_RageReadyEffect: hidden, non-hostile neutral script-archetype marker, no visuals, no script required.
- 0x804 SPEL KR_ActivationRequestSpell: free, self fire-and-forget, neutral effect 0x805 for 1 second; never added to the player's spell list.
- 0x805 MGEF KR_ActivationRequestEffect: neutral marker, no gameplay effect; TESMagicEffectApplyEvent is native request entry.
- 0x806 EFSH KR_FullRageGlow: own copy of Dragonborn.esm local 0x21733 (verify type), references existing textures, no spell/armor effects.

## VMAD properties
KR_MainQuest script KR_Config: Widget (object, script type KR_Widget) -> QUST 0x801.
Player alias KR_PlayerAlias: Config -> QUST 0x800, Widget -> QUST 0x801, FullGlow -> EFSH 0x806.
KR_WidgetQuest script KR_Widget: no required custom properties; inherited SkyUI infrastructure must be wired according to SDK (widget runtime registry).

## Combat hit rule (0.3.0)
Sole accumulation input is the native `TESHitEvent`; the bridge registers one sink. There is no PO3 dependency, no combat-time growth, no 0.1-second dedup and no per-second budget.

- Award: +2 per accepted hit, clamped to `rage.maximum`. Default 100 therefore needs 50 accepted hits. Pending/raging do not block accumulation; there is no equipment gate.
- Accept: player is the aggressor; target is a living `Actor`, not the player, not a teammate / player- or teammate-commanded actor; target hostile to the player; hit not marked blocked.
- Reject at arrival: target already dead, or the arrival is paused/menu/loading/unreconciled/player dead or out of combat. Target alive at arrival and killed before the queued task runs is NOT rejected for queueing, but the handle and hostility/friendly checks still apply when the task runs.
- The rule entry is `RageState::TryAwardCombatHit(inCombat, suspended)`; the bridge owns target resolution, hostility/friendly filtering and the arrival/processing state checks. Arrival filters blocked/liveness/hostility; the queued task re-checks the player, UI/loading state and target handle plus hostility/friendly, but does not re-check the blocked flag (immutable, already filtered at capture).
- Queueing carries a generation guard and a hit context. Menus opening/closing and out-of-combat/death notifications bump the context, so a hit queued before them is dropped as `stale_context` when it runs (a menu opened and closed before the hit runs still drops it); this is context isolation, not dedup. Pending/raging transitions do not bump the context.
- Diagnostics use `KratosRage hit <result>` with separate `received`, rejection-reason, `awarded`, `at_maximum` and `became_full` categories. Each category logs at most once per second; sequence, generation/context, cause/target/source/projectile IDs, flags, awarded amount, current/maximum, category count and suppressed count are captured. Counters span the process lifetime. Logging never limits scoring.
- Actual health loss is not observed: an immune target that still emits an unblocked hit event counts. Special attacks that emit no event are not inferred from animation. Death/kill events award nothing, and old `Submit*` / `SubmitPlayerDamage` stay no-ops. Offline tests do not prove these engine semantics; first hit, fatal hit and special-attack coverage need in-game acceptance.

## Fixed Papyrus bridge (KR_Native, global native)
ConfigureRage(int maximum, int normalHit, int powerHit, int specialHit, int receivedHit, int kill, int maxGainPerSecond, float deniedInterval)
SubmitWeaponHit(ObjectReference target, Form source, Projectile projectile, int flags)
SubmitMagicHit(ObjectReference target, Form source, Projectile projectile)
SubmitProjectileHit(ObjectReference target, Form source, Projectile projectile)
SubmitIncomingHit(ObjectReference aggressor, Form source, Projectile projectile, bool powerAttack, bool sneakAttack, bool bashAttack, bool blocked)
SubmitKill(Actor victim, Actor killer)
TickCombat()
RefreshEquipmentState()
NotifyActivationAttempt(string control, bool rightAttack, bool forward, bool back, bool left, bool right)
ReconcileAfterLoad()
GetRage() -> int
GetRageMaximum() -> int
IsRaging() -> bool
IsReady() -> bool
IsEligible() -> bool
IsPending() -> bool
IsAvailable() -> bool

Native emits SKSE ModCallbackEvent: KR_RageChanged, KR_RageDenied, KR_RageBecameFull. Papyrus event signature (string eventName, string strArg, float numArg, Form sender). Query latest snapshot using getters; no authoritative state in UI.

The ConfigureRage gain and maxGainPerSecond arguments are parsed and clamped for compatibility but do not drive 0.3.0 accumulation; only maximum changes the cap (and thus how many +2 hits fill it). All Submit* natives and the C++ SubmitPlayerDamage are no-ops.

The 0.5-second alias heartbeat is persistent, not limited to pending/raging; it keeps sampling even out of combat. TickCombat() queues a state refresh, out-of-combat/death clear and transaction reconcile, and no longer grants rage by elapsed time. Native emits KR_RageChanged when an activation starts or is cancelled. The state refresh confirms the original rage effect, or cancels an unconfirmed request after its 0.5-second deadline. SKSE AddTask is not a frame-delay mechanism.

## Widget interface
Script KR_Widget extends SKI_WidgetBase.
Papyrus methods ApplySettings(float x,float y,float width,float height,int fillColor,int readyColor,float opacity,float outOfCombatDelay,float fadeDuration,float deniedFlashDuration), UpdateRage(), RefreshVisibility(), ShowTemporarily(float seconds=3.0), FlashDenied(), Initialize().
SWF path Interface/exported/widgets/KratosRage/meter.swf; GetWidgetSource() returns KratosRage/meter.swf.
SWF AVM1/AS2 methods setPercent(number 0..1), setReady(bool), setStyle(fillColor,readyColor,width,height), showMeter(), hideMeter(), flashDenied(duration seconds), cancelAnimations(). Inherited x/y/alpha handled by SKI_WidgetBase. Keep no runtime resource logic in SWF.

## Configuration
SKSE/Plugins/StorageUtilData/KratosRage/config.json (JsonUtil filename KratosRage/config).
Fields: rage.maximum=100; rage.gain.normalHit=2,powerHit=2,specialHit=2,receivedHit=0,kill=0; rage.maxGainPerSecond=10 (gain fields compatibility-retained; 0.3.0 awards a fixed +2 per accepted hit).
hud.x=105,y=710,width=96,height=4,fillColor=12728377,readyColor=15646021,alpha=90,outOfCombatDelay=3.0,fadeDuration=0.4 (the always-on widget keeps outOfCombatDelay/fadeDuration parsed but not effective; HUD stays visible, ten-step, no numbers).
feedback.deniedFlashDuration=0.35,deniedInterval=0.5,fullGlowEnabled=true,fullGlowDuration=1.0.

The runtime config files (`config.json`, OAR/PI configs), ESP/SEQ, PEX and SWF are byte-for-byte reused from 0.2.0 and must not be regenerated for 0.3.0; only the DLL changes. The SWF still logs its component version as 0.2.0.

Do not edit original mod files, install into MO2, change profiles, run the game or desktop UI. Write only project files explicitly assigned to you. Git backup follows the user-approved plan: commit locally, do not push. Do not claim runtime verification. Communicate contract mismatches instead of silently changing shared interfaces.
