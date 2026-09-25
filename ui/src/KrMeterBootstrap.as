// ------------------------------------------------------------------------------------------------
// SWF entry point for the KratosRage meter.
//
// SkyUI's widgetloader loads this SWF into a holder clip and then expects a child MovieClip named
// "widget" on the loaded root (WidgetLoader.as: a_widgetHolder.widget). This entry point builds
// that clip and binds the public API to a KratosRageMeter instance, so the two visibility layers
// behave exactly like a SkyUI widget built in Flash:
//
//   loader holder clip (mode gating, _visible)  ->  "widget" clip (combat gating, alpha, geometry)
//
// MTASC's -main entry point calls the static main() of the class that defines it and passes the
// SWF root clip as the argument, which is exactly the clip the host loaded us into.
//
// Compiled by tools/build_ui.py:  mtasc -main -header 1280:720:30 ... -out build/ui/meter.swf
//
// setValue stays bound for older PEX callers but no longer renders diagnostic numbers.
// ------------------------------------------------------------------------------------------------

class KrMeterBootstrap
{
	static function main(a_root:MovieClip):Void
	{
		var widgetClip:MovieClip = a_root.createEmptyMovieClip("widget", 1);
		var meter:KratosRageMeter = new KratosRageMeter(widgetClip);

		// Keep the behaviour object alive for the lifetime of the clip.
		widgetClip.meter = meter;

		// ---- SKI_WidgetBase / WidgetLoader interface -------------------------------------------
		widgetClip.setHudMetrics = function(a_metrics:Object):Void { meter.setHudMetrics(a_metrics); };
		widgetClip.setRootPath = function(a_path:String):Void { meter.setRootPath(a_path); };
		widgetClip.setClientInfo = function(a_info:String):Void { meter.setClientInfo(a_info); };
		widgetClip.setHAnchor = function(a_anchor:String):Void { meter.setHAnchor(a_anchor); };
		widgetClip.setVAnchor = function(a_anchor:String):Void { meter.setVAnchor(a_anchor); };
		widgetClip.setPositionX = function(a_value:Number):Void { meter.setPositionX(a_value); };
		widgetClip.setPositionY = function(a_value:Number):Void { meter.setPositionY(a_value); };
		widgetClip.setAlpha = function(a_value:Number):Void { meter.setAlpha(a_value); };
		widgetClip.tweenTo = function(a_x:Number, a_y:Number, a_duration:Number):Void { meter.tweenTo(a_x, a_y, a_duration); };
		widgetClip.fadeTo = function(a_alpha:Number, a_duration:Number):Void { meter.fadeTo(a_alpha, a_duration); };
		widgetClip.onModeChange = function(a_hudMode:String):Void { meter.onModeChange(a_hudMode); };

		// setModes is called with a variable number of arguments (UI.InvokeStringA).
		widgetClip.setModes = function():Void
		{
			var modes:Array = [];
			for (var i:Number = 0; i < arguments.length; i++)
				modes.push(arguments[i]);
			meter.setModes.apply(meter, modes);
		};

		// ---- KR_Widget.psc interface -----------------------------------------------------------
		widgetClip.setStyle = function(a_fillColor:Number, a_readyColor:Number, a_width:Number, a_height:Number):Void
		{
			meter.setStyle(a_fillColor, a_readyColor, a_width, a_height);
		};
		widgetClip.setPercent = function(a_percent:Number):Void { meter.setPercent(a_percent); };
		widgetClip.setReady = function(a_ready:Boolean):Void { meter.setReady(a_ready); };
		widgetClip.setValue = function(a_current:Number, a_maximum:Number):Void { meter.setValue(a_current, a_maximum); };
		widgetClip.showMeter = function():Void { meter.showMeter(); };
		widgetClip.hideMeter = function():Void { meter.hideMeter(); };
		widgetClip.flashDenied = function(a_duration:Number):Void { meter.flashDenied(a_duration); };
		widgetClip.cancelAnimations = function():Void { meter.cancelAnimations(); };
	}
}
