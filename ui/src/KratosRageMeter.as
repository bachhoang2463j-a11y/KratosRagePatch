// ------------------------------------------------------------------------------------------------
// KratosRage meter widget - SWF side (AVM1 / ActionScript 2).
//
// Loaded by SkyUI's skyui/widgetloader.swf through KR_Widget.GetWidgetSource():
//   Interface/exported/widgets/KratosRage/meter.swf
//
// Contract with the host (see vendor/skyui-sdk/as for the reference implementation this mirrors):
//   * the loaded SWF root must expose a child MovieClip named "widget"                (WidgetLoader.onLoadInit)
//   * that clip must answer the SKI_WidgetBase invokes:
//       setHudMetrics, setRootPath, setClientInfo, setModes, setHAnchor, setVAnchor,
//       setPositionX, setPositionY, setAlpha, tweenTo, fadeTo, onModeChange
//   * and the widget API invoked from KR_Widget.psc:
//       setPercent, setReady, setStyle, setValue, showMeter, hideMeter, flashDenied, cancelAnimations
//
// setValue is retained as a no-op for older PEX callers. Fill is displayed in 10% steps;
// the actual percent and readiness remain authoritative.
//
// No gameplay state lives here: the bar only renders what Papyrus pushes. All decisions
// (when to show, when rage is ready, when an attempt is denied) stay on the Papyrus/native side.
// ------------------------------------------------------------------------------------------------

class KratosRageMeter
{
	// HUD modes the host may ask for; unknown names are ignored.
	private static var ALL_MODES:Array = [
		"All", "Favor", "MovementDisabled", "Swimming", "WarHorseMode", "HorseMode",
		"InventoryMode", "BookMode", "DialogueMode", "StealthMode", "SleepWaitMode",
		"BarterMode", "TweenMode", "WorldMapMode", "JournalMode", "CartMode", "VATSPlayback"
	];

	private static var BORDER_ALPHA:Number = 70;
	private static var BACKGROUND:Number = 0x171C20;
	private static var BORDER:Number = 0xAAB3BC;
	private static var FLASH_COLOR:Number = 0xC00000;
	private static var PULSE_MIN_ALPHA:Number = 55;
	private static var PULSE_PERIOD_MS:Number = 900;

	// Reported in the single per-load skse.Log line; bump together with the release version.
	private static var VERSION:String = "0.2.0";

	// stage
	private var _clip:MovieClip;      // "widget": the clip the host talks to
	private var _holder:MovieClip;    // clip that contains this SWF (SkyUI's widget holder)
	private var _bg:MovieClip;
	private var _fill:MovieClip;
	private var _flash:MovieClip;

	// style / state pushed from Papyrus
	private var _width:Number = 96;
	private var _height:Number = 4;
	private var _fillColor:Number = 0xC23839;
	private var _readyColor:Number = 0xEEBD45;
	private var _alpha:Number = 90;
	private var _percent:Number = 0;
	private var _ready:Boolean = false;
	private var _shown:Boolean = false;

	// host provided
	private var _hudMetrics:Object;
	private var _rootPath:String = "";
	private var _clientInfo:String = "";
	private var _hAnchor:String = "left";
	private var _vAnchor:String = "top";
	private var _posX:Number = 0;
	private var _posY:Number = 0;


	public function KratosRageMeter(a_clip:MovieClip)
	{
		_clip = a_clip;
		_holder = a_clip._parent;

		createStage();
		_clip._visible = false;
		_clip._alpha = 0;

		// Same starting state as the SkyUI widget base class: hidden until the manager hands out
		// the HUD modes, which is what makes setModes() flip the holder visible.
		if (_holder != undefined)
			_holder._visible = false;

		redraw();
	}


	// ==== host interface: SkyUI widget base ======================================================

	public function setHudMetrics(a_metrics:Object):Void
	{
		_hudMetrics = a_metrics;

		// 0.1.1 diagnostic: exactly one line per load, never per frame. hMin/hMax/vMin/vMax are in
		// _root-local units - the position mapping below must convert 1280/720 design coordinates
		// into that span, so a non-720 vertical span (e.g. a 16:10 logical frame) has to be visible.
		if (_global.skse != undefined && _global.skse.Log != undefined)
		{
			if (a_metrics == undefined)
				_global.skse.Log("KratosRage meter " + VERSION + " hud metrics: undefined");
			else
				_global.skse.Log("KratosRage meter " + VERSION + " hud metrics: hMin=" + a_metrics.hMin + " hMax=" + a_metrics.hMax + " vMin=" + a_metrics.vMin + " vMax=" + a_metrics.vMax);
		}

		updateAnchor();
		updatePosition();
	}

	public function setRootPath(a_path:String):Void
	{
		_rootPath = a_path;
	}

	public function setClientInfo(a_clientInfo:String):Void
	{
		_clientInfo = a_clientInfo;
	}

	public function setModes():Void
	{
		if (_holder == undefined)
			return;

		// Mirrors WidgetBase.setModes: mode gating lives on the holder as one boolean property per
		// valid HUD mode, so both this widget and the host can decide visibility with hasOwnProperty.
		clearHolderModeFlags();

		for (var i:Number = 0; i < arguments.length; i++)
		{
			var mode:String = resolveMode(arguments[i]);
			if (mode != null)
				_holder[mode] = true;
		}

		applyModeVisibility();

		// The HUD framework re-broadcasts mode changes to every element it knows about. The loader
		// already installed holder.onModeChange -> widget.onModeChange, so the holder answers the
		// broadcast from here on.
		var hud:Object = _root.HUDMovieBaseInstance;
		if (hud != undefined && hud.HudElements != undefined)
			hud.HudElements.push(_holder);
	}

	public function onModeChange(a_hudMode:String):Void
	{
		applyModeVisibility();
	}

	public function setHAnchor(a_anchor:String):Void
	{
		_hAnchor = a_anchor.toLowerCase();
		updateAnchor();
	}

	public function setVAnchor(a_anchor:String):Void
	{
		_vAnchor = a_anchor.toLowerCase();
		updateAnchor();
	}

	public function setPositionX(a_positionX:Number):Void
	{
		_posX = a_positionX;
		updatePosition();
	}

	public function setPositionY(a_positionY:Number):Void
	{
		_posY = a_positionY;
		updatePosition();
	}

	public function setAlpha(a_alpha:Number):Void
	{
		_alpha = clamp(a_alpha, 0, 100);
		if (_clip.onEnterFrame == undefined)
			_clip._alpha = _alpha;
	}

	public function tweenTo(a_newX:Number, a_newY:Number, a_duration:Number):Void
	{
		_posX = a_newX;
		_posY = a_newY;

		if (!hasMetrics() || a_duration <= 0)
		{
			updatePosition();
			return;
		}

		var fromX:Number = _clip._x;
		var fromY:Number = _clip._y;
		var toX:Number = lerp(-_hudMetrics.hMin, _hudMetrics.hMax, 0, 1280, _posX);
		var toY:Number = lerp(-_hudMetrics.vMin, _hudMetrics.vMax, 0, 720, _posY);
		var started:Number = getTimer();
		var durationMs:Number = a_duration * 1000;
		var meter:KratosRageMeter = this;

		_clip.onEnterFrame = function():Void
		{
			var t:Number = (getTimer() - started) / durationMs;
			if (t > 1)
				t = 1;
			this._x = fromX + (toX - fromX) * t;
			this._y = fromY + (toY - fromY) * t;
			if (t >= 1)
			{
				delete this.onEnterFrame;
				meter.onTweenDone();
			}
		};
	}

	public function fadeTo(a_alpha:Number, a_duration:Number):Void
	{
		var target:Number = clamp(a_alpha, 0, 100);

		// A fade always supersedes a running tween, and vice versa: only one enterFrame per clip.
		var started:Number = getTimer();
		var from:Number = _clip._alpha;
		var durationMs:Number = a_duration * 1000;
		var meter:KratosRageMeter = this;

		if (durationMs <= 0 || from == target)
		{
			delete _clip.onEnterFrame;
			_clip._alpha = target;
			onFadeDone();
			return;
		}

		_clip.onEnterFrame = function():Void
		{
			var t:Number = (getTimer() - started) / durationMs;
			if (t > 1)
				t = 1;
			this._alpha = from + (target - from) * t;
			if (t >= 1)
			{
				delete this.onEnterFrame;
				meter.onFadeDone();
			}
		};
	}


	// ==== widget API (invoked from KR_Widget.psc) =================================================

	public function setStyle(a_fillColor:Number, a_readyColor:Number, a_width:Number, a_height:Number):Void
	{
		_fillColor = Math.round(a_fillColor);
		_readyColor = Math.round(a_readyColor);
		if (a_width > 0)
			_width = a_width;
		if (a_height > 0)
			_height = a_height;

		redraw();
		updateAnchor();
	}

	public function setPercent(a_percent:Number):Void
	{
		var displayed:Number = getDisplayPercent();
		_percent = clamp(a_percent, 0, 1);
		if (displayed != getDisplayPercent())
			drawFill();
	}

	public function setReady(a_ready:Boolean):Void
	{
		if (_ready == a_ready)
			return;
		_ready = a_ready;
		drawFill();

		if (_ready)
			startPulse();
		else
			stopPulse();
	}

	// Older PEX callers can still invoke this method; numeric rendering has been removed.
	public function setValue(a_current:Number, a_maximum:Number):Void
	{
	}

	public function showMeter():Void
	{
		_shown = true;
		_clip._visible = true;
	}

	public function hideMeter():Void
	{
		_shown = false;
		delete _clip.onEnterFrame;
		_clip._alpha = 0;
		_clip._visible = false;
	}

	public function flashDenied(a_duration:Number):Void
	{
		if (a_duration == undefined)
			return;

		var durationMs:Number = a_duration * 1000;
		if (!(durationMs > 0))
			return;

		var started:Number = getTimer();
		_flash._visible = true;

		// Blink the whole strip red for the configured window, then restore the fill.
		_flash.onEnterFrame = function():Void
		{
			var elapsed:Number = getTimer() - started;
			if (elapsed >= durationMs)
			{
				this._alpha = 0;
				this._visible = false;
				delete this.onEnterFrame;
				return;
			}
			// Two quick full-intensity blinks, then a fade for the remainder.
			var phase:Number = (elapsed / durationMs) * 3;
			this._alpha = (phase < 2) ? ((phase % 1 < 0.5) ? 100 : 25) : (100 * (1 - ((phase - 2) / 1)));
		};
	}

	public function cancelAnimations():Void
	{
		delete _clip.onEnterFrame;
		delete _fill.onEnterFrame;
		delete _flash.onEnterFrame;
		_flash._alpha = 0;
		_flash._visible = false;
		_fill._alpha = 100;
	}


	// ==== internals ===============================================================================

	public function onFadeDone():Void
	{
		if (_clip._alpha <= 0)
		{
			_clip._visible = false;
			_shown = false;
		}
	}

	public function onTweenDone():Void
	{
		updatePosition();
	}

	private function createStage():Void
	{
		_bg = _clip.createEmptyMovieClip("bg", 1);
		_fill = _clip.createEmptyMovieClip("fill", 2);
		_flash = _clip.createEmptyMovieClip("flash", 3);
		_flash._visible = false;
		_flash._alpha = 0;
	}

	private function drawShape(a_clip:MovieClip, a_x:Number, a_y:Number, a_width:Number, a_height:Number):Void
	{
		var bevel:Number = Math.min(a_height, a_width / 4);
		a_clip.moveTo(a_x + bevel, a_y);
		a_clip.lineTo(a_x + a_width, a_y);
		a_clip.lineTo(a_x + a_width - bevel, a_y + a_height);
		a_clip.lineTo(a_x, a_y + a_height);
		a_clip.lineTo(a_x + bevel, a_y);
	}

	private function redraw():Void
	{
		_bg.clear();
		_bg.beginFill(0x070A0D, 90);
		drawShape(_bg, -2, -2, _width + 4, _height + 4);
		_bg.endFill();

		_bg.beginFill(BACKGROUND, 100);
		_bg.lineStyle(1, BORDER, BORDER_ALPHA);
		drawShape(_bg, -1, -1, _width + 2, _height + 2);
		_bg.endFill();

		_flash.clear();
		_flash.beginFill(FLASH_COLOR, 100);
		drawShape(_flash, 0, 0, _width, _height);
		_flash.endFill();

		drawFill();
	}

	private function getDisplayPercent():Number
	{
		if (!(_percent > 0))
			return 0;
		if (_percent >= 1)
			return 1;

		// Papyrus float rounding must not move an exact 10% boundary into the next step.
		var step:Number = Math.ceil(_percent * 10 - 0.00001);
		return clamp(step, 1, 9) / 10;
	}

	private function drawFill():Void
	{
		var fillWidth:Number = _width * getDisplayPercent();

		_fill.clear();
		if (fillWidth <= 0)
			return;

		var color:Number = _ready ? _readyColor : _fillColor;
		_fill.beginFill(color, 100);
		drawShape(_fill, 0, 0, fillWidth, _height);
		_fill.endFill();
	}

	private function startPulse():Void
	{
		var started:Number = getTimer();
		// Class statics are not visible inside the closure, so both are captured as locals.
		var period:Number = PULSE_PERIOD_MS;
		var minAlpha:Number = PULSE_MIN_ALPHA;

		_fill.onEnterFrame = function():Void
		{
			var elapsed:Number = getTimer() - started;
			var phase:Number = (elapsed % period) / period;
			var wave:Number = (phase < 0.5) ? phase * 2 : (1 - phase) * 2;
			this._alpha = minAlpha + (100 - minAlpha) * wave;
		};
	}

	private function stopPulse():Void
	{
		delete _fill.onEnterFrame;
		_fill._alpha = 100;
	}

	private function applyModeVisibility():Void
	{
		if (_holder == undefined)
			return;

		// Same test as WidgetBase: the holder answers for the current HUD mode via the flags written
		// by setModes. Mode changes arrive through the loader's holder.onModeChange forwarder ->
		// widget.onModeChange -> here, so menus keep hiding the bar while "All" shows it.
		// Mode gating lives on the holder clip, show/hide on the widget clip: two independent
		// layers, exactly like the SkyUI widget base class.
		_holder._visible = _holder.hasOwnProperty(currentHudMode());
	}

	private function clearHolderModeFlags():Void
	{
		for (var i:Number = 0; i < ALL_MODES.length; i++)
			delete _holder[ALL_MODES[i]];
	}

	private function currentHudMode():String
	{
		var hud:Object = _root.HUDMovieBaseInstance;
		if (hud == undefined || hud.HUDModes == undefined || hud.HUDModes.length == 0)
			return "All";
		return hud.HUDModes[hud.HUDModes.length - 1];
	}

	private function resolveMode(a_mode:String):String
	{
		if (a_mode == undefined)
			return null;

		for (var i:Number = 0; i < ALL_MODES.length; i++)
		{
			if (ALL_MODES[i].toLowerCase() == String(a_mode).toLowerCase())
				return ALL_MODES[i];
		}
		return null;
	}

	private function updateAnchor():Void
	{
		if (_holder == undefined)
			return;

		if (_hAnchor == "right")
			_holder._x = -_width;
		else if (_hAnchor == "center")
			_holder._x = -_width / 2;
		else
			_holder._x = 0;

		if (_vAnchor == "bottom")
			_holder._y = -_height;
		else if (_vAnchor == "center")
			_holder._y = -_height / 2;
		else
			_holder._y = 0;

		updatePosition();
	}

	private function updatePosition():Void
	{
		if (!hasMetrics())
		{
			_clip._x = _posX;
			_clip._y = _posY;
			return;
		}
		_clip._x = lerp(-_hudMetrics.hMin, _hudMetrics.hMax, 0, 1280, _posX);
		_clip._y = lerp(-_hudMetrics.vMin, _hudMetrics.vMax, 0, 720, _posY);
		// Keep the two-unit frame inside the safe HUD span, including anchor offsets.
		_clip._x = clamp(_clip._x, -_hudMetrics.hMin - _holder._x + 3, _hudMetrics.hMax - _holder._x - _width - 3);
		_clip._y = clamp(_clip._y, -_hudMetrics.vMin - _holder._y + 3, _hudMetrics.vMax - _holder._y - _height - 3);
	}

	private function hasMetrics():Boolean
	{
		return _hudMetrics != undefined && _hudMetrics.hMin != undefined;
	}

	// Mirrors Shared.GlobalFunc.Lerp(aTargetMin, aTargetMax, aSourceMin, aSourceMax, aSource): the
	// first pair is the OUTPUT range and the second pair is the INPUT range. Both call sites below
	// (updatePosition, tweenTo) use that SDK argument order verbatim. Swapping the two ranges is
	// numerically identical only while the host metrics are exactly 0..1280 / 0..720, which is why a
	// 16:10 logical frame (vMax != 720) exposed the wrong mapping.
	private function lerp(a_targetMin:Number, a_targetMax:Number, a_sourceMin:Number, a_sourceMax:Number, a_value:Number):Number
	{
		if (a_sourceMax == a_sourceMin)
			return a_targetMin;
		return a_targetMin + (a_value - a_sourceMin) * (a_targetMax - a_targetMin) / (a_sourceMax - a_sourceMin);
	}

	private function clamp(a_value:Number, a_min:Number, a_max:Number):Number
	{
		if (a_value < a_min)
			return a_min;
		if (a_value > a_max)
			return a_max;
		return a_value;
	}
}
