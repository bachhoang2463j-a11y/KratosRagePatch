# KratosRagePatch implementation contracts

Runtime plugin: KratosRagePatch.esp (ESL-compatible, form version 44), DLL KratosRage.dll, runtime 1.6.640 only.

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

## Fixed Papyrus bridge (KR_Native, global native)
ConfigureRage(int maximum, int normalHit, int powerHit, int specialHit, int receivedHit, int kill, int maxGainPerSecond, float deniedInterval)
SubmitWeaponHit(ObjectReference target, Form source, Projectile projectile, int flags)
SubmitMagicHit(ObjectReference target, Form source, Projectile projectile)
SubmitProjectileHit(ObjectReference target, Form source, Projectile projectile)
SubmitIncomingHit(ObjectReference aggressor, Form source, Projectile projectile, bool powerAttack, bool sneakAttack, bool bashAttack, bool blocked)
SubmitKill(Actor victim, Actor killer)
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

The existing 0.5-second single-update chain runs only while IsPending() or IsRaging(). Native emits KR_RageChanged when an activation starts or is cancelled. RefreshEquipmentState confirms the original rage effect, or cancels an unconfirmed request after its 0.5-second deadline. SKSE AddTask is not a frame-delay mechanism.

## Widget interface
Script KR_Widget extends SKI_WidgetBase.
Papyrus methods ApplySettings(float x,float y,float width,float height,int fillColor,int readyColor,float opacity,float outOfCombatDelay,float fadeDuration,float deniedFlashDuration), UpdateRage(), RefreshVisibility(), ShowTemporarily(float seconds=3.0), FlashDenied(), Initialize().
SWF path Interface/exported/widgets/KratosRage/meter.swf; GetWidgetSource() returns KratosRage/meter.swf.
SWF AVM1/AS2 methods setPercent(number 0..1), setReady(bool), setStyle(fillColor,readyColor,width,height), showMeter(), hideMeter(), flashDenied(duration seconds), cancelAnimations(). Inherited x/y/alpha handled by SKI_WidgetBase. Keep no runtime resource logic in SWF.

## Configuration
SKSE/Plugins/StorageUtilData/KratosRage/config.json (JsonUtil filename KratosRage/config).
Fields: rage.maximum=100; rage.gain.normalHit=2,powerHit=4,specialHit=3,receivedHit=1,kill=5; rage.maxGainPerSecond=8.
hud.x=80,y=650,width=180,height=9,fillColor=12728377,readyColor=15646021,alpha=90,outOfCombatDelay=3.0,fadeDuration=0.4.
feedback.deniedFlashDuration=0.35,deniedInterval=0.5,fullGlowEnabled=true,fullGlowDuration=1.0.

Do not edit original mod files, install into MO2, change profiles, run the game or desktop UI. Write only project files explicitly assigned to you. No Git commit/push. Do not claim runtime verification. Communicate contract mismatches instead of silently changing shared interfaces.
