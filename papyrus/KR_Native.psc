Scriptname KR_Native Hidden

; -------------------------------------------------------------------------------------------------
; Fixed native bridge implemented by KratosRage.dll (SSE 1.6.640 only).
;
; Direction of data:
;   Papyrus -> native : state reporting (Submit*, ConfigureRage, NotifyActivationAttempt,
;                       RefreshEquipmentState, ReconcileAfterLoad).
;   native -> Papyrus : SKSE ModCallbackEvent "KR_RageChanged" / "KR_RageDenied" /
;                       "KR_RageBecameFull" (signature: string eventName, string strArg,
;                       float numArg, Form sender). The events carry no authoritative state;
;                       subscribers query the getters below.
;
; This script owns no state and is never attached to a form (Hidden).
; Do not change a signature here without the DLL side changing in the same commit.
; -------------------------------------------------------------------------------------------------


; ==== state reporting (Papyrus -> native) ========================================================

; Pushes the effective configuration. Called once per game load with the values resolved by
; KR_Config (file value when present and valid, built-in default otherwise).
Function ConfigureRage(int maximum, int normalHit, int powerHit, int specialHit, int receivedHit, int kill, int maxGainPerSecond, float deniedInterval) global native

; Legacy hit entry points remain bound for saves made with older versions; they no longer score.
Function SubmitWeaponHit(ObjectReference target, Form source, Projectile projectile, int flags) global native

; Outgoing magic hit of the player (PO3 OnMagicHit).
Function SubmitMagicHit(ObjectReference target, Form source, Projectile projectile) global native

; Outgoing projectile hit of the player (PO3 OnProjectileHit).
Function SubmitProjectileHit(ObjectReference target, Form source, Projectile projectile) global native

; Incoming hit on the player (PO3 OnHitEx). aggressor may be None.
Function SubmitIncomingHit(ObjectReference aggressor, Form source, Projectile projectile, bool powerAttack, bool sneakAttack, bool bashAttack, bool blocked) global native

; The player killed victim (PO3 OnActorKilled).
Function SubmitKill(Actor victim, Actor killer) global native

; Samples player combat state on the main thread; the native timer owns all rage gain.
Function TickCombat() global native

; The player's equipment changed (OnObjectEquipped / OnObjectUnequipped on the player alias).
Function RefreshEquipmentState() global native

; Reports one input event on the given control (Skyrim control name, see Input.psc) together with
; the states of the other inputs that are observable from Papyrus. The native decides whether this
; is an activation attempt; it never grants anything on its own.
Function NotifyActivationAttempt(string control, bool rightAttack, bool forward, bool back, bool left, bool right) global native

; Called after a save is loaded, when the native restore has already happened.
Function ReconcileAfterLoad() global native


; ==== read-only snapshot =========================================================================

int Function GetRage() global native
int Function GetRageMaximum() global native
bool Function IsRaging() global native
bool Function IsReady() global native
bool Function IsEligible() global native

; True while an activation request is still awaiting confirmation or its timeout. The caller keeps
; its single-shot re-sync chain alive while this is set, so a request that never confirms still
; reaches the native side; the native alone decides whether the resource is consumed or rolled back.
bool Function IsPending() global native

; False when the native bridge or the required plugin records are unavailable.
; Callers must degrade silently instead of pretending the feature is available.
bool Function IsAvailable() global native
