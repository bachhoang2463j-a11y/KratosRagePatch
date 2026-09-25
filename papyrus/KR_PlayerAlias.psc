Scriptname KR_PlayerAlias extends ReferenceAlias

; -------------------------------------------------------------------------------------------------
; Forced player alias (ID 0) of QUST KR_MainQuest (0x800).
;
; Owns the Papyrus -> native input and lifecycle bridge:
;   * one single-shot heartbeat samples player combat state, without hit-event dependencies;
;   * the observed input control reports activation attempts, with the movement state that is
;     readable from Papyrus, so the native side can validate without inventing key bindings;
;   * equipment changes and save loads re-sync the native snapshot;
;   * the same heartbeat confirms pending activations and observes when rage ends.
;
; VMAD properties of this alias: Config -> QUST 0x800, Widget -> QUST 0x801, FullGlow -> EFSH 0x806.
; This alias also plays the full-rage glow, because it is the script that holds the 0x806 form:
; EffectShader.Play(ObjectReference, float duration) and Stop(ObjectReference) are vanilla natives,
; so no DLL is involved. Only that one shader form is ever touched, always for the player.
;
; Every registration below is idempotent: the unregister call is cheap and is issued right before the
; register call, so OnInit + OnAliasInit + every player load leave exactly one registration behind.
; -------------------------------------------------------------------------------------------------

KR_Config Property Config Auto
KR_Widget Property Widget Auto
EffectShader Property FullGlow Auto

string Property AttackControl = "Right Attack/Block" AutoReadOnly
string Property ForwardControl = "Forward" AutoReadOnly
string Property BackControl = "Back" AutoReadOnly
string Property StrafeLeftControl = "Strafe Left" AutoReadOnly
string Property StrafeRightControl = "Strafe Right" AutoReadOnly


; ==== lifecycle ==================================================================================

Event OnInit()
	EnsureBridge()
EndEvent

Event OnAliasInit()
	EnsureBridge()
EndEvent

Event OnPlayerLoadGame()
	; Saved timer deadlines belong to the previous process.
	UnregisterForUpdate()
	_rageWatchArmed = False
	_nextWatchAt = 0.0
	_watchInitialized = False
	EnsureBridge()

	if (KR_Native.IsAvailable())
		KR_Native.ReconcileAfterLoad()
	endIf

	if (Config)
		Config.HandlePlayerLoad()
	endIf

	if (Widget)
		; SKI_QuestBase.OnGameReload is SkyUI's reload hook (the one SKI_PlayerLoadGameAlias drives
		; for SkyUI's own quests): it re-subscribes the widget to "SKIWF_widgetManagerReady", which a
		; save load drops. Idempotent - the base only re-registers the callback and re-checks its
		; version, and OnWidgetInit is guarded by its own _initialized flag.
		Widget.OnGameReload()
		Widget.RefreshVisibility()
	endIf
EndEvent


; ==== registration ===============================================================================

Function EnsureBridge()
	if (KR_Native.IsAvailable())
		RegisterInputControls()
		RegisterRageEvents()
	endIf
	ArmRageWatch()
EndFunction

; KR_RageBecameFull and KR_RageChanged are emitted by the native bridge (see
; docs/implementation-contracts.md). The HUD prompt is handled by KR_Config; this alias owns the
; effect shader side of the full event and the end-of-rage watch.
Function RegisterRageEvents()
	string fullEvent = "KR_RageBecameFull"
	string changedEvent = "KR_RageChanged"

	UnregisterForModEvent(fullEvent)
	UnregisterForModEvent(changedEvent)
	RegisterForModEvent(fullEvent, "OnRageBecameFull")
	RegisterForModEvent(changedEvent, "OnRageChanged")
EndFunction

Function RegisterPoiEvents()
	; Retained for saved stacks from old versions; hit events no longer grant rage.
EndFunction

Function RegisterInputControls()
	; Only the activation control is observed. Registering movement controls here would report
	; ordinary walking as activation attempts, which is what the native side must never see.
	; Unregister only this script's own control: the player reference is shared with every other
	; mod, so UnregisterForAllControls could clear their registrations too.
	UnregisterForControl(AttackControl)
	RegisterForControl(AttackControl)
EndFunction


; ==== PO3 alias events ===========================================================================

Event OnWeaponHit(ObjectReference akTarget, Form akSource, Projectile akProjectile, int aiHitFlagMask)
	KR_Native.SubmitWeaponHit(akTarget, akSource, akProjectile, aiHitFlagMask)
EndEvent

Event OnMagicHit(ObjectReference akTarget, Form akSource, Projectile akProjectile)
	KR_Native.SubmitMagicHit(akTarget, akSource, akProjectile)
EndEvent

Event OnProjectileHit(ObjectReference akTarget, Form akSource, Projectile akProjectile)
	KR_Native.SubmitProjectileHit(akTarget, akSource, akProjectile)
EndEvent

Event OnHitEx(ObjectReference akAggressor, Form akSource, Projectile akProjectile, bool abPowerAttack, bool abSneakAttack, bool abBashAttack, bool abHitBlocked)
	KR_Native.SubmitIncomingHit(akAggressor, akSource, akProjectile, abPowerAttack, abSneakAttack, abBashAttack, abHitBlocked)
EndEvent

Event OnActorKilled(Actor akVictim, Actor akKiller)
	KR_Native.SubmitKill(akVictim, akKiller)
EndEvent


; ==== full-rage feedback (effect shader) =========================================================

; Fired by the native bridge when rage reaches its maximum. Plays this mod's own effect shader on
; the player for the configured time. Nothing global is touched: Stop and Play are always called on
; the FullGlow form (EFSH 0x806) for the player only, so no other mod's shader is affected.
Event OnRageBecameFull(string eventName, string strArg, float numArg, Form sender)
	if (!FullGlow || !Config)
		; VMAD reference missing: no glow, the rest of the mod keeps working.
		return
	endIf

	if (!Config.AllowFullFeedback())
		; A save was just loaded: the native restore can re-emit this for a state that was already
		; full when the game was saved. That is not a new full-rage event.
		return
	endIf

	if (!Config.GlowEnabled)
		return
	endIf

	float duration = Config.GlowDuration
	if (duration <= 0.0)
		return
	endIf

	Actor player = Game.GetPlayer()
	if (player == None)
		return
	endIf

	; Stop first so a second "became full" cannot stack a second instance of our own shader.
	FullGlow.Stop(player)
	FullGlow.Play(player, duration)

	; The transformation may start together with the full event.
	ArmRageWatch()
EndEvent


; ==== combat and rage heartbeat ==================================================================

; Player OnCombatStateChanged is not reliable. Keep one low-frequency chain alive even outside combat.
float Property RageEndWatchInterval = 0.5 AutoReadOnly

bool _rageWatchArmed = False
bool _watchInitialized = False
float _nextWatchAt = 0.0

Function ArmRageWatch()
	if (_rageWatchArmed)
		return
	endIf

	_rageWatchArmed = True
	_nextWatchAt = Utility.GetCurrentRealTime() + RageEndWatchInterval
	RegisterForSingleUpdate(RageEndWatchInterval)
EndFunction

Event OnRageChanged(string eventName, string strArg, float numArg, Form sender)
	ArmRageWatch()
EndEvent

Event OnUpdate()
	float now = Utility.GetCurrentRealTime()
	if (_rageWatchArmed && now < _nextWatchAt)
		RegisterForSingleUpdate(_nextWatchAt - now)
		return
	endIf
	_rageWatchArmed = False

	if (KR_Native.IsAvailable())
		if (!_watchInitialized)
			_watchInitialized = True
			RegisterInputControls()
			RegisterRageEvents()
			if (Config)
				Config.LoadConfiguration()
			endIf
		endIf
		KR_Native.TickCombat()
	endIf
	ArmRageWatch()
EndEvent


; ==== vanilla alias events =======================================================================

Event OnObjectEquipped(Form akBaseObject, ObjectReference akReference)
	RefreshEquipment()
EndEvent

Event OnObjectUnequipped(Form akBaseObject, ObjectReference akReference)
	RefreshEquipment()
EndEvent

Event OnCombatStateChanged(Actor akTarget, int aeCombatState)
	; Combat start/end is the only trigger the HUD needs for its show / fade-out policy.
	if (Widget)
		Widget.RefreshVisibility()
	endIf
EndEvent

Function RefreshEquipment()
	if (!KR_Native.IsAvailable())
		return
	endIf

	KR_Native.RefreshEquipmentState()

	if (Widget)
		Widget.RefreshVisibility()
	endIf
EndFunction


; ==== input ======================================================================================

; The native side owns the decision; this only reports that the attack control was pressed together
; with the input state that is actually readable from Papyrus. Nothing is reported while the main
; mechanic is unavailable or the player cannot be eligible at all, so the API never looks "usable"
; when it is not.
Event OnControlDown(string control)
	if (control != AttackControl)
		return
	endIf

	if (!KR_Native.IsAvailable() || !KR_Native.IsEligible())
		return
	endIf

	KR_Native.NotifyActivationAttempt(control, IsControlPressed(AttackControl), IsControlPressed(ForwardControl), IsControlPressed(BackControl), IsControlPressed(StrafeLeftControl), IsControlPressed(StrafeRightControl))
EndEvent

bool Function IsControlPressed(string control)
	int keyCode = Input.GetMappedKey(control)
	if (keyCode <= 0)
		return False
	endIf
	return Input.IsKeyPressed(keyCode)
EndFunction
