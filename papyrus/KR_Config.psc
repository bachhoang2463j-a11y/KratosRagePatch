Scriptname KR_Config extends Quest

; -------------------------------------------------------------------------------------------------
; Read-only JSON configuration for the KratosRage patch.
;
; File: SKSE/Plugins/StorageUtilData/KratosRage/config.json  (JsonUtil name "KratosRage/config").
; The file is NEVER written back. A missing file, a parse error or a value outside its accepted
; range falls back to the built-in default for that single field only.
;
; Every field is read through the JsonUtil path API (".rage.maximum", see the field readers below):
; the plain Get*Value getters only look inside JsonUtil's own "int"/"float"/"string"/"form"
; containers, so on a hand-authored nested file like this one they would silently return the
; fallback for every field and the whole configuration would appear to be ignored.
;
; Attach to QUST KR_MainQuest (0x800); VMAD property Widget -> QUST KR_WidgetQuest (0x801).
; -------------------------------------------------------------------------------------------------

KR_Widget Property Widget Auto
{KR_Widget instance (QUST 0x801). Without it the HUD stays off; everything else keeps working.}

string Property JsonFile = "KratosRage/config" AutoReadOnly

; Event names emitted by the native bridge.
string Property EventRageChanged = "KR_RageChanged" AutoReadOnly
string Property EventRageDenied = "KR_RageDenied" AutoReadOnly
string Property EventRageBecameFull = "KR_RageBecameFull" AutoReadOnly

; Built-in defaults. Must stay identical to data/config.default.json.
int Property DefaultMaximum = 100 AutoReadOnly
int Property DefaultNormalHit = 2 AutoReadOnly
int Property DefaultPowerHit = 2 AutoReadOnly
int Property DefaultSpecialHit = 2 AutoReadOnly
int Property DefaultReceivedHit = 0 AutoReadOnly
int Property DefaultKill = 0 AutoReadOnly
int Property DefaultMaxGainPerSecond = 10 AutoReadOnly
float Property DefaultDeniedInterval = 0.5 AutoReadOnly
float Property DefaultHudX = 80.0 AutoReadOnly
float Property DefaultHudY = 650.0 AutoReadOnly
float Property DefaultHudWidth = 180.0 AutoReadOnly
float Property DefaultHudHeight = 9.0 AutoReadOnly
int Property DefaultFillColor = 12728377 AutoReadOnly
int Property DefaultReadyColor = 15646021 AutoReadOnly
float Property DefaultHudAlpha = 90.0 AutoReadOnly
float Property DefaultOutOfCombatDelay = 3.0 AutoReadOnly
float Property DefaultFadeDuration = 0.4 AutoReadOnly
float Property DefaultDeniedFlashDuration = 0.35 AutoReadOnly
bool Property DefaultFullGlowEnabled = True AutoReadOnly
float Property DefaultFullGlowDuration = 1.0 AutoReadOnly

; Last configuration actually pushed to the native side (read-only view for diagnostics).
int Property AppliedMaximum Auto
float Property AppliedFullGlowDuration Auto

; Effect shader feedback resolved from the JSON, consumed by KR_PlayerAlias (which owns the EFSH).
; Plain fields behind read-only properties: no VMAD property is added by this.
bool _glowEnabled = True
float _glowDuration = 1.0

; Real time of the last save load, 0.0 while the session has not loaded one. See AllowFullFeedback.
float _loadedAt = 0.0

; Grace period after a save load during which the full-rage feedback stays silent. Long enough for
; ReconcileAfterLoad and the native restore to settle, short enough not to swallow a real event.
float Property FullFeedbackLoadGrace = 2.0 AutoReadOnly

; The native side drops its configured flag when a save is restored and when a new game is started,
; and only ConfigureRage re-arms it (without it eligibility can never become true). OnInit runs in
; the same start-up as that new-game reset, so the push is repeated once after start-up; it is the
; same idempotent push, and a save load never waits for it (OnPlayerLoadGame pushes directly).
float Property StartupRepushDelay = 1.0 AutoReadOnly

bool Property GlowEnabled
	bool function get()
		return _glowEnabled
	endFunction
endProperty

float Property GlowDuration
	float function get()
		return _glowDuration
	endFunction
endProperty


; ==== lifecycle ==================================================================================

Event OnInit()
	Initialize()
EndEvent

Function Initialize()
	LoadConfiguration()

	; One-shot. If the native new-game reset happened after this OnInit, the configuration pushed
	; above was wiped with it; this second, identical push re-arms it. No chain is registered for
	; the save-load case, which is already covered by KR_PlayerAlias.OnPlayerLoadGame.
	RegisterForSingleUpdate(StartupRepushDelay)
EndFunction

Event OnUpdate()
	LoadConfiguration()
EndEvent

; Called by KR_PlayerAlias.OnPlayerLoadGame. Re-reads the file (it is a plain file the user may edit
; between sessions) and re-registers the callbacks. Idempotent: no state is duplicated.
Function HandlePlayerLoad()
	_loadedAt = Utility.GetCurrentRealTime()
	LoadConfiguration()
EndFunction

; False right after a save load. KR_RageBecameFull is an edge signal from the native side, but a
; restored save can re-emit it for a rage state that was already full when the game was saved; a
; replay would look like a fresh full-rage event (HUD prompt and glow both react to it). Both
; consumers ask here, so the policy lives in one place.
bool Function AllowFullFeedback()
	if (_loadedAt <= 0.0)
		return True
	endIf
	return Utility.GetCurrentRealTime() - _loadedAt > FullFeedbackLoadGrace
EndFunction


; ==== configuration ==============================================================================

Function LoadConfiguration()
	; Re-read from the file every time this runs: it is plain user-editable JSON, a read never
	; writes it back, and JsonUtil drops its cached copy when a save is loaded, so this is what
	; refreshes the values for the session that is starting. Load() reports whether the file parsed;
	; a file that is absent is not an error.
	bool fileUsable = False
	if (JsonUtil.JsonExists(JsonFile))
		fileUsable = JsonUtil.Load(JsonFile)
	endIf

	if (fileUsable)
		Debug.Trace("[KR_Config] reading " + JsonFile + ".json")
	else
		Debug.Trace("[KR_Config] " + JsonFile + ".json missing or not parseable, using defaults")
	endIf

	int maximum = ReadInt("rage.maximum", DefaultMaximum, 1, 10000, fileUsable)
	int normalHit = ReadInt("rage.gain.normalHit", DefaultNormalHit, 0, 10000, fileUsable)
	int powerHit = ReadInt("rage.gain.powerHit", DefaultPowerHit, 0, 10000, fileUsable)
	int specialHit = ReadInt("rage.gain.specialHit", DefaultSpecialHit, 0, 10000, fileUsable)
	int receivedHit = ReadInt("rage.gain.receivedHit", DefaultReceivedHit, 0, 10000, fileUsable)
	int kill = ReadInt("rage.gain.kill", DefaultKill, 0, 10000, fileUsable)
	int maxGainPerSecond = ReadInt("rage.maxGainPerSecond", DefaultMaxGainPerSecond, 1, 10000, fileUsable)
	float deniedInterval = ReadFloat("feedback.deniedInterval", DefaultDeniedInterval, 0.0, 60.0, fileUsable)

	float hudX = ReadFloat("hud.x", DefaultHudX, -1280.0, 1280.0, fileUsable)
	float hudY = ReadFloat("hud.y", DefaultHudY, -720.0, 720.0, fileUsable)
	float hudWidth = ReadFloat("hud.width", DefaultHudWidth, 1.0, 1280.0, fileUsable)
	float hudHeight = ReadFloat("hud.height", DefaultHudHeight, 1.0, 720.0, fileUsable)
	int fillColor = ReadColor("hud.fillColor", DefaultFillColor, fileUsable)
	int readyColor = ReadColor("hud.readyColor", DefaultReadyColor, fileUsable)
	float hudAlpha = ReadFloat("hud.alpha", DefaultHudAlpha, 0.0, 100.0, fileUsable)
	float outOfCombatDelay = ReadFloat("hud.outOfCombatDelay", DefaultOutOfCombatDelay, 0.0, 600.0, fileUsable)
	float fadeDuration = ReadFloat("hud.fadeDuration", DefaultFadeDuration, 0.0, 60.0, fileUsable)
	float deniedFlashDuration = ReadFloat("feedback.deniedFlashDuration", DefaultDeniedFlashDuration, 0.0, 60.0, fileUsable)

	; The 1 s full-rage glow is played by KR_PlayerAlias, which is the script holding the 0x806 EFSH
	; VMAD property (EffectShader.Play is a vanilla native, no DLL involved). The resolved values are
	; handed over through GlowEnabled / GlowDuration.
	_glowEnabled = ReadBool("feedback.fullGlowEnabled", DefaultFullGlowEnabled, fileUsable)
	_glowDuration = ReadFloat("feedback.fullGlowDuration", DefaultFullGlowDuration, 0.0, 60.0, fileUsable)

	AppliedMaximum = maximum
	AppliedFullGlowDuration = _glowDuration

	KR_Native.ConfigureRage(maximum, normalHit, powerHit, specialHit, receivedHit, kill, maxGainPerSecond, deniedInterval)

	if (Widget)
		Widget.ApplySettings(hudX, hudY, hudWidth, hudHeight, fillColor, readyColor, hudAlpha, outOfCombatDelay, fadeDuration, deniedFlashDuration)
		Widget.UpdateRage()
		Widget.RefreshVisibility()
	else
		Debug.Trace("[KR_Config] Widget VMAD property is empty, HUD disabled")
	endIf

	if (!_glowEnabled)
		Debug.Trace("[KR_Config] feedback.fullGlowEnabled = false, no full-rage glow")
	endIf

	RegisterCallbacks()
EndFunction


; ==== native events ==============================================================================

; Idempotent: every entry point unregisters before registering, so repeated OnInit / player-load
; cycles never stack duplicate callbacks.
Function RegisterCallbacks()
	UnregisterForModEvent(EventRageChanged)
	UnregisterForModEvent(EventRageDenied)
	UnregisterForModEvent(EventRageBecameFull)

	RegisterForModEvent(EventRageChanged, "OnKrRageChanged")
	RegisterForModEvent(EventRageDenied, "OnKrRageDenied")
	RegisterForModEvent(EventRageBecameFull, "OnKrRageBecameFull")
EndFunction

Event OnKrRageChanged(string eventName, string strArg, float numArg, Form sender)
	RefreshWidget()
EndEvent

Event OnKrRageDenied(string eventName, string strArg, float numArg, Form sender)
	if (Widget)
		Widget.FlashDenied()
	endIf
EndEvent

Event OnKrRageBecameFull(string eventName, string strArg, float numArg, Form sender)
	if (!AllowFullFeedback())
		; A save was just loaded: not a new full-rage edge, do not replay the prompt.
		return
	endIf

	if (Widget)
		Widget.UpdateRage()
		Widget.ShowTemporarily(3.0)
	endIf
EndEvent

Function RefreshWidget()
	if (Widget)
		Widget.UpdateRage()
		Widget.RefreshVisibility()
	endIf
EndFunction


; ==== field readers (validate, fall back to the default, never write back) ========================
;
; Note: the parameter is named keyName because Papyrus identifiers are case-insensitive and "key"
; would collide with the vanilla Key form type. The JsonUtil path API takes the dotted path with a
; leading dot and matches the members of the file exactly, so the keys below have to stay identical
; to the ones in data/config.default.json (compile_papyrus.py enforces that pairing).

int Function ReadInt(string keyName, int fallback, int minValue, int maxValue, bool fileUsable)
	if (!fileUsable)
		return fallback
	endIf
	int fieldValue = JsonUtil.GetPathIntValue(JsonFile, "." + keyName, fallback)
	if (fieldValue < minValue || fieldValue > maxValue)
		Debug.Trace("[KR_Config] " + keyName + " out of range, using default")
		return fallback
	endIf
	return fieldValue
EndFunction

float Function ReadFloat(string keyName, float fallback, float minValue, float maxValue, bool fileUsable)
	if (!fileUsable)
		return fallback
	endIf
	float fieldValue = JsonUtil.GetPathFloatValue(JsonFile, "." + keyName, fallback)
	if (fieldValue < minValue || fieldValue > maxValue)
		Debug.Trace("[KR_Config] " + keyName + " out of range, using default")
		return fallback
	endIf
	return fieldValue
EndFunction

bool Function ReadBool(string keyName, bool fallback, bool fileUsable)
	if (!fileUsable)
		return fallback
	endIf
	return JsonUtil.GetPathBoolValue(JsonFile, "." + keyName, fallback)
EndFunction

int Function ReadColor(string keyName, int fallback, bool fileUsable)
	if (!fileUsable)
		return fallback
	endIf
	int fieldValue = JsonUtil.GetPathIntValue(JsonFile, "." + keyName, fallback)
	if (fieldValue < 0 || fieldValue > 16777215)
		Debug.Trace("[KR_Config] " + keyName + " is not an RGB colour, using default")
		return fallback
	endIf
	return fieldValue
EndFunction
