Scriptname KR_Widget extends SKI_WidgetBase

; -------------------------------------------------------------------------------------------------
; SkyUI widget for the KratosRage bar (thin strip).
;
; Attach to QUST KR_WidgetQuest (0x801), start-game enabled. No VMAD property is required for the
; registration itself: SKI_WidgetBase subscribes to SkyUI's "SKIWF_widgetManagerReady" mod event and
; the widget manager then loads GetWidgetSource() through skyui/widgetloader.swf. See
; docs/implementation-contracts.md and the VMAD note reported to the ESP generator:
;   - the quest must be start-game enabled and running, otherwise OnInit never fires and the widget
;     never registers;
;   - the only VMAD property this script wants is on KR_Config (Widget -> QUST 0x801).
;
; HUD policy owned here:
;   * diagnostic build: show whenever the native bridge is available, including at zero rage;
;   * menu handling is delegated to SkyUI (SetModes / HUD mode gating in the SWF).
; No rage value is stored here beyond the last rendered snapshot; KR_Native stays authoritative.
; -------------------------------------------------------------------------------------------------

; ==== configured style ===========================================================================

float _width = 180.0
float _height = 9.0
int _fillColor = 12728377
int _readyColor = 15646021
float _opacity = 90.0
float _outOfCombatDelay = 3.0
float _fadeDuration = 0.4
float _deniedFlashDuration = 0.35

; ==== rendered state =============================================================================

float _percent = 0.0
bool _ready = False
bool _displayed = False
float _temporaryUntil = 0.0
float _hideNotBefore = 0.0
float _nextCheckAt = 0.0
bool _checkArmed = False

; 0.1.1 diagnostic. While this is True the bar is shown whenever KR_Native answers, with no combat,
; eligibility or raging gate and without the out-of-combat fade. Set it to False to restore the
; shipping visibility policy; nothing else in this script depends on it.
bool _diagnosticAlwaysOn = True

float Property DefaultX = 80.0 AutoReadOnly
float Property DefaultY = 650.0 AutoReadOnly
int Property DefaultFillColor = 12728377 AutoReadOnly
int Property DefaultReadyColor = 15646021 AutoReadOnly

; Default grace period before the widget is armed when a denied flash needs it on screen.
float Property DefaultTemporarySeconds = 3.0 AutoReadOnly


; ==== SKI_WidgetBase interface ===================================================================

; SKI_WidgetBase.OnGameReload (reached from its own OnInit) pops a modal Debug.MessageBox when
; IsExtending() is false and RequireExtend is true. IsExtending() compares
; StringUtil.Substring(self as string, 1, ...) against GetWidgetType() + " ", which matches for a
; widget whose script name equals its widget type - exactly what GetWidgetType() documents
; ("Must be the same as scriptname"). Clearing the flag before the base's OnInit runs keeps the
; documented GetWidgetType() and still loads without a message box. This override is the only hook
; that runs earlier than that check.
Event OnInit()
	RequireExtend = False
	parent.OnInit()
EndEvent

; SKI_WidgetBase.OnGameReload is the reload hook: the base reaches it from its own OnInit, and
; KR_PlayerAlias calls it on every player load. The three deadlines below are absolute
; Utility.GetCurrentRealTime() values, and that timer restarts with the process: a deadline saved
; by a longer session would otherwise keep the bar on screen, or postpone its fade, until the
; fresh session happens to pass the same second again.
Event OnGameReload()
	_temporaryUntil = 0.0
	_hideNotBefore = 0.0
	_nextCheckAt = 0.0
	_checkArmed = False
	_diagnosticAlwaysOn = True

	parent.OnGameReload()
EndEvent

Event OnWidgetInit()
	WidgetName = "Kratos Rage"
	X = DefaultX
	Y = DefaultY
EndEvent

Event OnWidgetReset()
	parent.OnWidgetReset()

	; The SWF instance is brand new (or was just reloaded): re-push everything it cannot know.
	ApplyStyleToSwf()
	UpdateRage()
	ApplyDisplayToSwf()

	RefreshVisibility()
EndEvent

string Function GetWidgetSource()
	return "KratosRage/meter.swf"
EndFunction

string Function GetWidgetType()
	; Must match the scriptname.
	return "KR_Widget"
EndFunction

float[] Function GetDimensions()
	float[] dim = new float[2]
	dim[0] = _width
	dim[1] = _height
	return dim
EndFunction


; ==== public API (called from KR_Config / KR_PlayerAlias) ========================================

; Pushes the configuration resolved by KR_Config. Safe to call repeatedly.
; Parameter names avoid the case-insensitive inherited X / Y / Alpha / Ready members on purpose.
Function ApplySettings(float barX, float barY, float barWidth, float barHeight, int fillColor, int readyColor, float opacityValue, float outOfCombatDelay, float fadeDuration, float deniedFlashDuration)
	_width = ClampFloat(barWidth, 1.0, 1280.0)
	_height = ClampFloat(barHeight, 1.0, 720.0)
	_fillColor = ClampColor(fillColor, DefaultFillColor)
	_readyColor = ClampColor(readyColor, DefaultReadyColor)
	_opacity = ClampFloat(opacityValue, 0.0, 100.0)
	_outOfCombatDelay = ClampFloat(outOfCombatDelay, 0.0, 600.0)
	_fadeDuration = ClampFloat(fadeDuration, 0.0, 60.0)
	_deniedFlashDuration = ClampFloat(deniedFlashDuration, 0.0, 60.0)

	; Inherited base properties; they reach the SWF through SKI_WidgetBase once Ready.
	X = ClampFloat(barX, -1280.0, 1280.0)
	Y = ClampFloat(barY, -720.0, 720.0)
	Alpha = _opacity

	Initialize()
EndFunction

; Re-pushes the current state to the SWF. Idempotent, no-op while the widget is not registered yet.
Function Initialize()
	ApplyStyleToSwf()
	PushRageToSwf()
	ApplyDisplayToSwf()
EndFunction

; Reads the authoritative snapshot from the native side and renders it.
Function UpdateRage()
	if (!KR_Native.IsAvailable())
		; Bridge missing: render an empty, not-ready bar.
		_percent = 0.0
		_ready = False
		PushRageToSwf()
		return
	endIf

	int rage = KR_Native.GetRage()
	int maximum = KR_Native.GetRageMaximum()

	float rageValue = rage
	float maximumValue = maximum

	float percent = 0.0
	if (maximum > 0)
		percent = rageValue / maximumValue
	endIf

	_percent = ClampFloat(percent, 0.0, 1.0)
	_ready = KR_Native.IsReady()

	PushRageToSwf()
EndFunction

; Re-evaluates whether the bar should be on screen right now.
Function RefreshVisibility()
	if (!Ready)
		return
	endIf

	if (ComputeWantVisible())
		_hideNotBefore = 0.0

		if (_temporaryUntil > 0.0)
			float temporaryLeft = _temporaryUntil - Utility.GetCurrentRealTime()
			if (temporaryLeft <= 0.0)
				; The timed full-rage show has run out and something else keeps the bar up. Forget
				; it, so it cannot later skip the out-of-combat grace that case is entitled to.
				_temporaryUntil = 0.0
			else
				; Never leave the end of the timed show unwatched: an earlier out-of-combat check
				; may already have fired and consumed the only pending shot, which would keep the
				; bar on screen until some unrelated event happens to call back in.
				ArmCheck(temporaryLeft)
			endIf
		endIf

		if (!_displayed)
			ShowMeterInternal()
		endIf
		return
	endIf

	if (!_displayed)
		return
	endIf

	if (_temporaryUntil > 0.0)
		; Nothing but the timed show (rage became full) kept the bar on screen, and the timer has
		; run out: that timer *was* the visible window. Hide now; stacking the out-of-combat grace
		; on top would keep it up for a second, unintended delay.
		_temporaryUntil = 0.0
		HideMeterInternal()
		return
	endIf

	float now = Utility.GetCurrentRealTime()
	if (_hideNotBefore <= 0.0)
		_hideNotBefore = now + _outOfCombatDelay
	endIf

	if (now < _hideNotBefore)
		; Out of combat grace period; hide later unless something shows it again.
		ArmCheck(_hideNotBefore - now)
		return
	endIf

	HideMeterInternal()
EndFunction

; Shows the bar for a moment even outside combat (used when rage becomes full).
Function ShowTemporarily(float seconds = 3.0)
	float duration = ClampFloat(seconds, 0.1, 600.0)
	_temporaryUntil = Utility.GetCurrentRealTime() + duration
	_hideNotBefore = 0.0

	if (!_displayed)
		ShowMeterInternal()
	endIf

	if (Ready)
		ArmCheck(duration)
	endIf
EndFunction

; Red flash for a rejected activation attempt. Never fakes availability.
Function FlashDenied()
	if (!Ready || !KR_Native.IsAvailable())
		return
	endIf

	if (!_displayed)
		_hideNotBefore = 0.0
		ShowMeterInternal()
		ArmCheck(_outOfCombatDelay)
	endIf

	UI.InvokeFloat(HUD_MENU, WidgetRoot + ".flashDenied", _deniedFlashDuration)
EndFunction


; ==== visibility decision ========================================================================

bool Function ComputeWantVisible()
	if (!KR_Native.IsAvailable())
		; Dependency missing: never pretend the feature is usable.
		return False
	endIf

	if (_diagnosticAlwaysOn)
		; 0.1.1 diagnostic: bypass the combat, eligibility and raging gate. The HUD mode holder
		; (SkyUI SetModes gating in the SWF) still applies, so menus keep hiding the bar, and this
		; branch is what keeps the out-of-combat fade out of the picture.
		return True
	endIf

	if (Utility.GetCurrentRealTime() < _temporaryUntil)
		return True
	endIf

	if (KR_Native.IsRaging())
		return True
	endIf

	if (!KR_Native.IsEligible())
		; Wrong / unsupported equipment: stay hidden.
		return False
	endIf

	Actor player = Game.GetPlayer()
	if (player == None)
		return False
	endIf

	return player.IsInCombat()
EndFunction

Function ShowMeterInternal()
	_displayed = True
	_hideNotBefore = 0.0

	if (!Ready)
		return
	endIf

	UI.Invoke(HUD_MENU, WidgetRoot + ".showMeter")
	UI.InvokeFloatA(HUD_MENU, WidgetRoot + ".fadeTo", FadeArgs(_opacity, _fadeDuration))
EndFunction

Function HideMeterInternal()
	_displayed = False
	_hideNotBefore = 0.0

	if (!Ready)
		return
	endIf

	; The SWF hides itself once the fade reaches zero.
	UI.InvokeFloatA(HUD_MENU, WidgetRoot + ".fadeTo", FadeArgs(0.0, _fadeDuration))
EndFunction

; Single-shot re-arming timer. Never a repeating loop: a check is only armed while something is
; pending (out-of-combat grace period or a temporary show).
Function ArmCheck(float delay)
	float now = Utility.GetCurrentRealTime()
	float at = now + delay

	if (_checkArmed && at >= _nextCheckAt)
		return
	endIf

	_nextCheckAt = at
	_checkArmed = True
	RegisterForSingleUpdate(delay)
EndFunction

Event OnUpdate()
	float now = Utility.GetCurrentRealTime()

	if (now < _nextCheckAt)
		; A newer, earlier check has been armed in the meantime.
		RegisterForSingleUpdate(_nextCheckAt - now)
		return
	endIf

	_checkArmed = False
	RefreshVisibility()
EndEvent


; ==== SWF plumbing ===============================================================================

Function ApplyStyleToSwf()
	if (!Ready)
		return
	endIf

	float[] args = new float[4]
	float fillValue = _fillColor
	float readyValue = _readyColor
	args[0] = fillValue
	args[1] = readyValue
	args[2] = _width
	args[3] = _height
	UI.InvokeFloatA(HUD_MENU, WidgetRoot + ".setStyle", args)
EndFunction

Function PushRageToSwf()
	if (!Ready)
		return
	endIf

	UI.InvokeFloat(HUD_MENU, WidgetRoot + ".setPercent", _percent)
	UI.InvokeBool(HUD_MENU, WidgetRoot + ".setReady", _ready)
EndFunction

; Applied when the SWF instance is (re)created: instant state, no tween.
Function ApplyDisplayToSwf()
	if (!Ready)
		return
	endIf

	UI.Invoke(HUD_MENU, WidgetRoot + ".cancelAnimations")

	float alphaValue = 0.0
	if (_displayed)
		alphaValue = _opacity
	endIf
	UI.InvokeFloat(HUD_MENU, WidgetRoot + ".setAlpha", alphaValue)

	if (_displayed)
		UI.Invoke(HUD_MENU, WidgetRoot + ".showMeter")
	else
		UI.Invoke(HUD_MENU, WidgetRoot + ".hideMeter")
	endIf
EndFunction

float[] Function FadeArgs(float targetAlpha, float duration)
	float[] args = new float[2]
	args[0] = targetAlpha
	args[1] = duration
	return args
EndFunction


; ==== helpers ====================================================================================

float Function ClampFloat(float value, float minValue, float maxValue)
	if (value < minValue)
		return minValue
	endIf
	if (value > maxValue)
		return maxValue
	endIf
	return value
EndFunction

int Function ClampColor(int value, int fallback)
	if (value < 0 || value > 16777215)
		return fallback
	endIf
	return value
EndFunction
