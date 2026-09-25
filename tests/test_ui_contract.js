'use strict';
/*
 * Offline contract test for the KratosRage HUD widget (node, no dependencies):
 *
 *     node tests/test_ui_contract.js
 *
 * It does NOT re-implement the widget. It reads the real sources, extracts the real method bodies
 * out of ui/src/KratosRageMeter.as and ui/src/KrMeterBootstrap.as, transpiles those bodies to
 * JavaScript (type annotations only) and runs them against a mock MovieClip / Stage / skse
 * environment. The expected coordinate mapping is an independent implementation of the documented
 * SkyUI contract (Shared.GlobalFunc.Lerp(targetMin, targetMax, sourceMin, sourceMax, value)), not a
 * copy of the project's own helper.
 *
 * What this can prove: mapping arithmetic, the stepped display-percent ladder, drawing invariants,
 * holder mode-flag protocol, bootstrap wiring and the Papyrus <-> SWF name contract.
 * What it cannot prove: anything visual. Scaleform rendering, font/alpha behaviour and the real
 * _hudMetrics values still need in-game acceptance.
 */

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const METER_AS = path.join(ROOT, 'ui', 'src', 'KratosRageMeter.as');
const BOOTSTRAP_AS = path.join(ROOT, 'ui', 'src', 'KrMeterBootstrap.as');
const WIDGET_PSC = path.join(ROOT, 'papyrus', 'KR_Widget.psc');

/* ------------------------------------------------------------------------------------------------
 * Source extraction
 * ---------------------------------------------------------------------------------------------- */

// Returns the body (without the enclosing braces) of `function <name>(...)` plus its parameter names.
function extractMethod(source, name) {
    const head = new RegExp('(?:^|\\n)[ \\t]*(?:(?:public|private|static)[ \\t]+)*function[ \\t]+' + name + '[ \\t]*\\(');
    const match = head.exec(source);
    if (!match) throw new Error('method not found: ' + name);

    let i = match.index + match[0].length; // just past '('
    let depth = 1;
    const params = [];
    let current = '';
    for (; i < source.length && depth > 0; i++) {
        const ch = source[i];
        if (ch === '(') depth++;
        else if (ch === ')') { depth--; if (depth === 0) { if (current.trim()) params.push(current.trim()); break; } }
        else if (ch === ',' && depth === 1) { params.push(current.trim()); current = ''; }
        else current += ch;
    }
    while (i < source.length && source[i] !== '{') i++; // skip the return type
    if (source[i] !== '{') throw new Error('body not found: ' + name);
    i++;

    let bodyDepth = 1;
    let body = '';
    for (; i < source.length; i++) {
        const ch = source[i];
        if (ch === '"' || ch === "'") {                       // skip string literals
            body += ch;
            const quote = ch;
            i++;
            for (; i < source.length; i++) {
                body += source[i];
                if (source[i] === '\\') { i++; body += source[i]; continue; }
                if (source[i] === quote) break;
            }
            continue;
        }
        if (ch === '/' && source[i + 1] === '/') {            // skip line comments
            for (; i < source.length && source[i] !== '\n'; i++) { }
            body += '\n';
            continue;
        }
        if (ch === '{') bodyDepth++;
        if (ch === '}') { bodyDepth--; if (bodyDepth === 0) break; }
        body += ch;
    }
    return { params: params.map((p) => p.split(':')[0].trim()), body };
}

// Evaluates the literal of `[private|public] static var <name>:Type = <literal>;`.
function extractStaticLiteral(source, name) {
    const head = new RegExp('static[ \\t]+var[ \\t]+' + name + '[ \\t]*:[ \\t]*[A-Za-z_][\\w.$]*[ \\t]*=');
    const match = head.exec(source);
    if (!match) throw new Error('static var not found: ' + name);

    let i = match.index + match[0].length;
    let depth = 0;
    let literal = '';
    for (; i < source.length; i++) {
        const ch = source[i];
        if (ch === '"') {
            literal += ch;
            i++;
            for (; i < source.length; i++) { literal += source[i]; if (source[i] === '\\') { i++; literal += source[i]; continue; } if (source[i] === '"') break; }
            continue;
        }
        if (ch === '[' || ch === '(' || ch === '{') depth++;
        if (ch === ']' || ch === ')' || ch === '}') depth--;
        if (ch === ';' && depth === 0) break;
        literal += ch;
    }
    return eval('(' + literal + ')'); // literal only: numbers, strings, array/object literals
}

const TYPE_ANNOTATION = /([\w)\]$])\s*:\s*(?:Void|Number|Boolean|String|Array|Object|MovieClip|KratosRageMeter|KrMeterBootstrap)\b/g;
const stripTypes = (code) => code.replace(TYPE_ANNOTATION, '$1');

// Papyrus is brace-free: collect from `Function <name>(` up to its `EndFunction`.
function extractPapyrusFunction(source, name) {
    const head = new RegExp('(?:^|\\n)[ \\t]*(?:[A-Za-z_][\\w]*[ \\t]+)?Function[ \\t]+' + name + '[ \\t]*\\(');
    const match = head.exec(source);
    if (!match) throw new Error('papyrus function not found: ' + name);
    const rest = source.slice(match.index);
    const end = /\n[ \t]*EndFunction/.exec(rest);
    if (!end) throw new Error('EndFunction not found for: ' + name);
    return rest.slice(0, end.index + end[0].length);
}

function compileMethod(target, name, source) {
    const { params, body } = extractMethod(source, name);
    const factory = new Function('__self', 'return function(' + params.join(', ') + '){ with(__self){' + stripTypes(body) + '} };');
    target[name] = factory(target);
    return params.length;
}

/* ------------------------------------------------------------------------------------------------
 * Mock Scaleform environment
 * ---------------------------------------------------------------------------------------------- */

function makeClip(name) {
    return {
        _name: name,
        _x: 0,
        _y: 0,
        _alpha: 100,
        _visible: true,
        _xscale: 100,
        _yscale: 100,
        ops: [],
        children: {},
        createEmptyMovieClip(childName) {
            const clip = makeClip(childName);
            this.children[childName] = clip;
            return clip;
        },
        clear() { this.ops.push(['clear']); },
        beginFill(color, alpha) { this.ops.push(['beginFill', color, alpha]); },
        moveTo(x, y) { this.ops.push(['moveTo', x, y]); },
        lineTo(x, y) { this.ops.push(['lineTo', x, y]); },
        endFill() { this.ops.push(['endFill']); },
        lineStyle(thickness, color, alpha) { this.ops.push(['lineStyle', thickness, color, alpha]); },
    };
}

function makeMeter(source) {
    const logs = [];
    const clip = makeClip('widget');
    const holder = makeClip('9');
    clip._parent = holder;

    const meter = {
        _clip: clip,
        _holder: holder,
        _bg: undefined,
        _fill: undefined,
        _flash: undefined,
        _hudMetrics: undefined,
        _rootPath: '',
        _clientInfo: '',
        _hAnchor: 'left',
        _vAnchor: 'top',
        _posX: 0,
        _posY: 0,
        _width: 96,
        _height: 4,
        _fillColor: 0xC23839,
        _readyColor: 0xEEBD45,
        _percent: 0,
        _ready: false,
        _shown: false,
        logs,
        _global: { skse: { Log: (message) => logs.push(message) } },
        _root: { HUDMovieBaseInstance: { HUDModes: ['All'], HudElements: [] } },
    };
    meter.meter = meter; // the bodies' `var meter = this` alias resolves through the with-scope
    meter.ALL_MODES = extractStaticLiteral(source, 'ALL_MODES');
    meter.VERSION = extractStaticLiteral(source, 'VERSION');
    meter.PULSE_PERIOD_MS = extractStaticLiteral(source, 'PULSE_PERIOD_MS');
    meter.PULSE_MIN_ALPHA = extractStaticLiteral(source, 'PULSE_MIN_ALPHA');
    meter.BORDER_ALPHA = extractStaticLiteral(source, 'BORDER_ALPHA');
    meter.BACKGROUND = extractStaticLiteral(source, 'BACKGROUND');
    meter.BORDER = extractStaticLiteral(source, 'BORDER');
    meter.FLASH_COLOR = extractStaticLiteral(source, 'FLASH_COLOR');

    let clock = 0;
    meter.getTimer = () => clock;
    meter.setClock = (value) => { clock = value; };

    [
        'lerp', 'clamp', 'hasMetrics', 'updatePosition', 'updateAnchor', 'setPositionX', 'setPositionY',
        'tweenTo', 'onTweenDone', 'setHudMetrics', 'setValue', 'getDisplayPercent', 'createStage',
        'setModes', 'clearHolderModeFlags', 'applyModeVisibility', 'currentHudMode', 'resolveMode',
        'onModeChange', 'setPercent', 'setReady', 'drawFill', 'redraw', 'startPulse', 'stopPulse',
        'drawShape', 'setHAnchor',
    ].forEach((name) => compileMethod(meter, name, source));

    // onModeChange delegates to applyModeVisibility; wire the real body.
    const { body: onModeBody } = extractMethod(source, 'onModeChange');
    meter.onModeChange = new Function('__self', 'return function(){ with(__self){' + stripTypes(onModeBody) + '} };')(meter);

    return meter;
}

// Runs one setPercent against a fresh ops buffer so the measurement only covers this draw.
function drawAt(meter, percent) {
    meter._fill.ops = [];
    meter.setPercent(percent);
    return meter._fill.ops;
}

// The horizontal extent the fill actually drew this time (0 when nothing was drawn).
function drawnWidth(ops) {
    for (const op of ops) {
        if (op[0] === 'lineTo' && op[2] === 0)
            return op[1];
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------
 * Independent contract oracle (documented SkyUI behaviour, not the project's helper)
 * ---------------------------------------------------------------------------------------------- */

// Shared.GlobalFunc.Lerp(aTargetMin, aTargetMax, aSourceMin, aSourceMax, aSource)
const sdkLerp = (targetMin, targetMax, sourceMin, sourceMax, value) =>
    targetMin + (value - sourceMin) * (targetMax - targetMin) / (sourceMax - sourceMin);
const expectedX = (metrics, designX) => sdkLerp(-metrics.hMin, metrics.hMax, 0, 1280, designX);
const expectedY = (metrics, designY) => sdkLerp(-metrics.vMin, metrics.vMax, 0, 720, designY);

// Independent restatement of the documented display ladder:
//   0 -> 0; (0, 10%] -> 10%; afterwards ceiling to the next 10% step; a not-really-full bar never
//   renders above 90%; only a real percent >= 1 renders 100%. The 1e-5 epsilon before the ceiling
//   absorbs float32 noise from the Papyrus float pipeline at exact .1/.2 boundaries.
function expectedDisplayPercent(percent) {
    if (!(percent > 0))
        return 0;
    if (percent >= 1)
        return 1;
    let steps = Math.ceil(percent * 10 - 1e-5);
    if (steps < 1)
        steps = 1;
    if (steps > 9)
        steps = 9;
    return steps / 10;
}

/* ------------------------------------------------------------------------------------------------
 * Tiny runner
 * ---------------------------------------------------------------------------------------------- */

let passed = 0;
const failures = [];
function check(name, condition, detail) {
    if (condition) { passed++; return; }
    failures.push(name + (detail ? ' :: ' + detail : ''));
}
const near = (a, b, epsilon) => Math.abs(a - b) <= (epsilon === undefined ? 1e-9 : epsilon);

/* ------------------------------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------------------------- */

const meterSource = fs.readFileSync(METER_AS, 'utf8');
const bootstrapSource = fs.readFileSync(BOOTSTRAP_AS, 'utf8');
const widgetSource = fs.readFileSync(WIDGET_PSC, 'utf8');

const METRIC_CASES = [
    { label: '1280x720 identity', metrics: { hMin: 0, hMax: 1280, vMin: 0, vMax: 720 } },
    { label: '16:10 logical frame 2560x1600', metrics: { hMin: 0, hMax: 1280, vMin: 0, vMax: 800 } },
    { label: 'letterboxed safe rect', metrics: { hMin: 0, hMax: 1280, vMin: 40, vMax: 760 } },
    { label: 'stretched high resolution', metrics: { hMin: 0, hMax: 1280, vMin: 0, vMax: 1080 } },
    { label: 'height-fit horizontal span', metrics: { hMin: 0, hMax: 1152, vMin: 0, vMax: 720 } },
];

// T1 - the real updatePosition must follow the SDK mapping for every metrics shape.
for (const testCase of METRIC_CASES) {
    const meter = makeMeter(meterSource);
    meter._hudMetrics = testCase.metrics;
    meter.setPositionX(80);
    meter.setPositionY(650);
    check('T1 x ' + testCase.label, near(meter._clip._x, expectedX(testCase.metrics, 80)),
        'got ' + meter._clip._x + ' expected ' + expectedX(testCase.metrics, 80));
    check('T1 y ' + testCase.label, near(meter._clip._y, expectedY(testCase.metrics, 650)),
        'got ' + meter._clip._y + ' expected ' + expectedY(testCase.metrics, 650));
}

// T1b - the 16:10 case must differ from the pre-fix argument order, and the identity case must not.
{
    const swapped = (value, inMin, inMax, outMin, outMax) =>
        outMin + (value - inMin) * (outMax - outMin) / (inMax - inMin);
    const sixteenTen = { hMin: 0, hMax: 1280, vMin: 0, vMax: 800 };
    const identity = { hMin: 0, hMax: 1280, vMin: 0, vMax: 720 };
    check('T1b swapped order is detected on 16:10',
        Math.abs(swapped(650, -sixteenTen.vMin, sixteenTen.vMax, 0, 720) - expectedY(sixteenTen, 650)) > 100);
    check('T1b swapped order is invisible on identity metrics',
        near(swapped(650, -identity.vMin, identity.vMax, 0, 720), expectedY(identity, 650)));
}

// T2 - tweenTo drives the same mapping (via the real onEnterFrame closure).
{
    const meter = makeMeter(meterSource);
    const metrics = METRIC_CASES[1].metrics;
    meter._hudMetrics = metrics;
    meter._clip._x = 0;
    meter._clip._y = 0;
    meter.setClock(0);
    meter.tweenTo(80, 650, 0.5);
    check('T2 tweenTo installs an enterFrame', typeof meter._clip.onEnterFrame === 'function');
    for (let step = 1; step <= 4 && typeof meter._clip.onEnterFrame === 'function'; step++) {
        meter.setClock(step * 200);
        meter._clip.onEnterFrame();
    }
    check('T2 tweenTo final x', near(meter._clip._x, expectedX(metrics, 80), 1e-6),
        'got ' + meter._clip._x + ' expected ' + expectedX(metrics, 80));
    check('T2 tweenTo final y', near(meter._clip._y, expectedY(metrics, 650), 1e-6),
        'got ' + meter._clip._y + ' expected ' + expectedY(metrics, 650));
    check('T2 tweenTo clears its enterFrame', meter._clip.onEnterFrame === undefined);
}

// T3 - the stage has no diagnostic value clip, and the legacy setValue entry point is a no-op.
{
    const meter = makeMeter(meterSource);
    meter.createStage();
    check('T3 stage builds the strip clips', meter._bg !== undefined && meter._fill !== undefined && meter._flash !== undefined);
    check('T3 stage creates no value clip',
        meter._clip.children.value === undefined && meter._value === undefined);

    // Legacy PEX compatibility: the old invoke must not throw and must not touch the render state.
    const percentBefore = meter._percent;
    const readyBefore = meter._ready;
    let threw = false;
    try {
        meter.setValue(37, 100);
        meter.setValue(undefined, undefined);
        meter.setValue(NaN, 5);
    } catch (error) {
        threw = true;
    }
    check('T3 legacy setValue does not throw', !threw);
    check('T3 legacy setValue does not touch percent', meter._percent === percentBefore);
    check('T3 legacy setValue does not touch ready', meter._ready === readyBefore);
    check('T3 legacy setValue draws nothing into the fill', meter._fill.ops.length === 0);
}

// T3b - the numeric readout machinery is gone from the source, and nothing scales or resizes the strip.
{
    for (const symbol of ['VALUE_COLOR', 'VALUE_CELL', 'VALUE_LEFT', 'VALUE_TOP', 'VALUE_CHAR_ADVANCE',
        'VALUE_GLYPH_ROWS', 'VALUE_GLYPH_COLS', 'drawValue', 'glyphFor', 'sanitizeValue', 'fillRect']) {
        check('T3b source no longer contains ' + symbol, !meterSource.includes(symbol));
    }
    // The numeric readout state is gone; `a_value` parameters of lerp/clamp are unrelated and allowed.
    const readoutState = /(^|[^A-Za-z0-9_])(_value|_valueCurrent|_valueMaximum)\b/;
    check('T3b no numeric readout state in the source', !readoutState.test(meterSource),
        (readoutState.exec(meterSource) || [''])[0]);
    check('T3b no _xscale/_yscale assignment in the widget source',
        !/\._xscale\s*=/.test(meterSource) && !/\._yscale\s*=/.test(meterSource));
    check('T3b no _clip._width/_height assignment',
        !/_clip\._width\s*=/.test(meterSource) && !/_clip\._height\s*=/.test(meterSource));
}

// T4 - holder mode flags and visibility follow WidgetBase.setModes.
{
    const meter = makeMeter(meterSource);
    meter._root.HUDMovieBaseInstance.HUDModes = ['All'];
    meter.setModes('All', 'StealthMode', 'notAMode');
    check('T4 valid mode flag set', meter._holder.All === true);
    check('T4 second valid mode flag set', meter._holder.StealthMode === true);
    check('T4 unknown mode ignored', meter._holder.notAMode === undefined);
    check('T4 only requested flags exist', meter._holder.InventoryMode === undefined && meter._holder.CartMode === undefined);
    check('T4 holder visible in All', meter._holder._visible === true);
    check('T4 holder registered with the HUD', meter._root.HUDMovieBaseInstance.HudElements.indexOf(meter._holder) !== -1);

    meter._root.HUDMovieBaseInstance.HUDModes = ['InventoryMode'];
    meter.onModeChange('InventoryMode');
    check('T4 menu mode hides the holder', meter._holder._visible === false);

    meter._root.HUDMovieBaseInstance.HUDModes = ['All'];
    meter.onModeChange('All');
    check('T4 returning to All shows the holder again', meter._holder._visible === true);

    meter.setModes('All');
    check('T4 stale mode flags are cleared', meter._holder.StealthMode === undefined && meter._holder.All === true);

    meter.setModes('notAMode');
    check('T4 no valid mode means hidden', meter._holder._visible === false && meter._holder.All === undefined);
}

// T5 - setHudMetrics logs once per load and re-applies the mapping.
{
    const meter = makeMeter(meterSource);
    const metrics = METRIC_CASES[1].metrics;
    meter.setHudMetrics(metrics);
    check('T5 metrics log line', meter.logs.length === 1 && /\b0\.2\.0\b/.test(meter.logs[0]) && /vMax=800/.test(meter.logs[0]),
        JSON.stringify(meter.logs));
    check('T5 metrics stored', meter._hudMetrics === metrics);

    const bare = makeMeter(meterSource);
    bare.setHudMetrics(undefined);
    check('T5 missing metrics are logged, not thrown', bare.logs.length === 1 && /undefined/.test(bare.logs[0]));
}

// T10 - the stepped display ladder: _percent keeps the real value, the fill width follows
// getDisplayPercent() through the real drawFill body.
{
    // [input percent, expected display fraction]
    const LADDER = [
        [0, 0],
        [0.0001, 0.1],
        [0.01, 0.1],
        [0.02, 0.1],
        [0.1, 0.1],
        [Math.fround(0.1), 0.1],   // float32 .1 is slightly above .1: still one step, not two
        [0.1001, 0.2],
        [0.2, 0.2],
        [0.81, 0.9],
        [0.9, 0.9],
        [0.99, 0.9],
        [0.9999, 0.9],             // not really full: never 100%
        [1, 1],                    // only a real 1 renders full width
    ];

    for (const [input, expectedFraction] of LADDER) {
        const meter = makeMeter(meterSource);
        meter.createStage();

        const ops = drawAt(meter, input);
        const width = drawnWidth(ops);
        const expectedWidth = meter._width * expectedFraction;

        check('T10 percent kept raw for ' + input, meter._percent === input,
            'got ' + meter._percent);
        check('T10 display width for ' + input + ' is ' + expectedFraction, near(width, expectedWidth, 1e-6),
            'got ' + width + ' expected ' + expectedWidth);
        check('T10 oracle agrees for ' + input, expectedFraction === expectedDisplayPercent(input));
        if (input < 1)
            check('T10 under ' + input + ' not full width', width < meter._width, 'got ' + width);
        if (input > 0)
            check('T10 above ' + input + ' not empty', width > 0, 'got ' + width);
    }

    // The direct accessor agrees with what the fill drew (same source of truth, no hidden second path).
    {
        const meter = makeMeter(meterSource);
        meter.createStage();
        meter.setPercent(0.81);
        check('T10 getDisplayPercent matches the ladder', near(meter.getDisplayPercent(), 0.9, 1e-9),
            'got ' + meter.getDisplayPercent());
    }

    // setPercent never fabricates readiness; gold still comes exclusively from setReady.
    {
        const meter = makeMeter(meterSource);
        meter.createStage();
        const ops = drawAt(meter, 0.81);
        check('T10 setPercent leaves ready false', meter._ready === false);
        check('T10 unfilled percent draws rage red',
            ops.some((op) => op[0] === 'beginFill' && op[1] === 0xC23839), JSON.stringify(ops));

        meter._fill.ops = [];
        meter.setReady(true);
        check('T10 setReady paints gold',
            meter._fill.ops.some((op) => op[0] === 'beginFill' && op[1] === 0xEEBD45), JSON.stringify(meter._fill.ops));

        meter._fill.ops = [];
        meter.setReady(false);
        check('T10 setReady(false) paints rage red again',
            meter._fill.ops.some((op) => op[0] === 'beginFill' && op[1] === 0xC23839), JSON.stringify(meter._fill.ops));
    }

    // Zero rage draws zero fill: no beginFill at all on this draw.
    {
        const meter = makeMeter(meterSource);
        meter.createStage();
        const ops = drawAt(meter, 0);
        check('T10 zero percent draws no fill', !ops.some((op) => op[0] === 'beginFill'), JSON.stringify(ops));
    }

    // NaN / Infinity: whatever the implementation does with them it must not throw, and a later
    // valid push must recover a normal draw.
    {
        const meter = makeMeter(meterSource);
        meter.createStage();
        let threw = false;
        try {
            meter.setPercent(NaN);
            meter.setPercent(Infinity);
            meter.setPercent(-Infinity);
        } catch (error) {
            threw = true;
        }
        check('T10 NaN/Infinity pushes do not throw', !threw);
        const ops = drawAt(meter, 0.5);
        check('T10 valid push after NaN/Infinity recovers', near(drawnWidth(ops), meter._width * 0.5, 1e-6),
            'got ' + drawnWidth(ops));
    }
}

// T6 - the bootstrap builds the host contract on a "widget" child and forwards variable arity.
{
    const { body } = extractMethod(bootstrapSource, 'main');
    const calls = [];
    const created = [];
    const root = {
        _name: '_root',
        children: {},
        createEmptyMovieClip(name, depth) {
            created.push({ name, depth });
            const clip = makeClip(name);
            clip._depth = depth;
            this.children[name] = clip;
            return clip;
        },
    };
    class StubMeter {
        constructor(clip) { this.clip = clip; }
        setValue(current, maximum) { calls.push(['setValue', current, maximum]); }
        setModes() { calls.push(['setModes'].concat(Array.prototype.slice.call(arguments))); }
        setHudMetrics(metrics) { calls.push(['setHudMetrics', metrics]); }
    }
    const runMain = new Function('__self', 'KratosRageMeter', 'return function(a_root){ with(__self){' + stripTypes(body) + '} };')({}, StubMeter);
    runMain(root);

    check('T6 widget child created at depth 1', created.length === 1 && created[0].name === 'widget' && created[0].depth === 1,
        JSON.stringify(created));
    const widgetClip = root.children.widget;
    check('T6 meter bound to the widget clip', widgetClip.meter instanceof StubMeter && widgetClip.meter.clip === widgetClip);

    const hostNames = ['setHudMetrics', 'setRootPath', 'setClientInfo', 'setModes', 'setHAnchor', 'setVAnchor',
        'setPositionX', 'setPositionY', 'setAlpha', 'tweenTo', 'fadeTo', 'onModeChange'];
    const widgetNames = ['setStyle', 'setPercent', 'setReady', 'setValue', 'showMeter', 'hideMeter', 'flashDenied', 'cancelAnimations'];
    for (const name of hostNames.concat(widgetNames)) {
        check('T6 widget clip exposes ' + name, typeof widgetClip[name] === 'function');
    }

    widgetClip.setValue(12, 34);
    check('T6 legacy setValue binding still forwards', calls.some((call) => call[0] === 'setValue' && call[1] === 12 && call[2] === 34), JSON.stringify(calls));
    widgetClip.setModes('All', 'Favor', 'CartMode');
    check('T6 setModes forwards variable arity', calls.some((call) => call[0] === 'setModes' && call[1] === 'All' && call[2] === 'Favor' && call[3] === 'CartMode'), JSON.stringify(calls));
    const metricsArg = { hMin: 0, hMax: 1280, vMin: 0, vMax: 800 };
    widgetClip.setHudMetrics(metricsArg);
    check('T6 host invoke reaches the meter', calls.some((call) => call[0] === 'setHudMetrics' && call[1] === metricsArg), JSON.stringify(calls));
}

// T7 - Papyrus <-> SWF name contract, derived from the real .psc text. The numeric setValue invoke
// is gone from Papyrus, while the SWF still answers it for old PEX compatibility.
{
    const invoked = new Set();
    const invokeRe = /WidgetRoot\s*\+\s*"\.([A-Za-z_][A-Za-z0-9_]*)"/g;
    let match;
    while ((match = invokeRe.exec(widgetSource)) !== null) invoked.add(match[1]);
    check('T7 papyrus no longer invokes setValue', !invoked.has('setValue'), [...invoked].join(','));
    check('T7 papyrus keeps the existing API', ['setStyle', 'setPercent', 'setReady', 'showMeter', 'hideMeter',
        'flashDenied', 'cancelAnimations', 'setAlpha', 'fadeTo'].every((name) => invoked.has(name)));

    const widgetClipNames = new Set();
    const bindRe = /widgetClip\.([A-Za-z_][A-Za-z0-9_]*)\s*=/g;
    while ((match = bindRe.exec(bootstrapSource)) !== null) widgetClipNames.add(match[1]);
    for (const name of invoked) {
        check('T7 bootstrap answers .' + name, widgetClipNames.has(name));
    }
    check('T7 bootstrap keeps the legacy setValue binding', widgetClipNames.has('setValue'));
}

// T8 - KR_Widget.psc pushes only percent/ready; no numeric snapshot invoke remains.
{
    const body = extractPapyrusFunction(widgetSource, 'PushRageToSwf');
    check('T8 pushes percent', /UI\.InvokeFloat\(HUD_MENU,\s*WidgetRoot\s*\+\s*"\.setPercent",\s*_percent\)/.test(body));
    check('T8 pushes ready', /UI\.Invoke(?:Bool|A)?\(HUD_MENU,\s*WidgetRoot\s*\+\s*"\.setReady"/.test(body));
    check('T8 no setValue invoke in PushRageToSwf', !/setValue/.test(body));
    check('T8 no float array in PushRageToSwf', !/new float\[/.test(body));
    check('T8 no numeric state fields remain', !widgetSource.includes('_rageCurrent') && !widgetSource.includes('_rageMaximum'));
}

// T9 - diagnostic visibility policy and the empty-bar fallback.
{
    check('T9 diagnostic flag declared', /bool\s+_diagnosticAlwaysOn\s*=\s*True/.test(widgetSource));

    const body = extractPapyrusFunction(widgetSource, 'ComputeWantVisible');
    const order = body.indexOf('_diagnosticAlwaysOn');
    check('T9 availability gate comes first', body.indexOf('KR_Native.IsAvailable()') < order);
    check('T9 diagnostic branch returns True', /if\s*\(_diagnosticAlwaysOn\)[\s\S]{0,600}?return True/.test(body));
    check('T9 combat gate is still behind the flag', body.indexOf('IsInCombat') > order);

    const update = extractPapyrusFunction(widgetSource, 'UpdateRage');
    const unavailable = update.slice(update.indexOf('IsAvailable'), update.indexOf('endIf'));
    check('T9 unavailable branch falls back to an empty bar',
        /_percent\s*=\s*0\.0/.test(unavailable) && /_ready\s*=\s*False/.test(unavailable));
    check('T9 available branch clamps the percent', /_percent\s*=\s*ClampFloat\(percent/.test(update));
    check('T9 ready still comes from the native side', /_ready\s*=\s*KR_Native\.IsReady\(\)/.test(update));
    check('T9 no per-frame update registration added', !/RegisterForUpdate\(/.test(widgetSource));
}

// T11 - 0.2.0 default geometry: w96 h4, design position (105, 710) in config + Papyrus + AS,
// and the SkyUI-mapped position survives the safe-span clamp on 16:9 and 16:10.
{
    const config = JSON.parse(fs.readFileSync(path.join(ROOT, 'data', 'config.default.json'), 'utf8'));
    check('T11 config default x is 105', config.hud.x === 105, 'got ' + config.hud.x);
    check('T11 config default y is 710', config.hud.y === 710, 'got ' + config.hud.y);
    check('T11 config default size is 96x4', config.hud.width === 96 && config.hud.height === 4);
    check('T11 papyrus DefaultX is 105', /float\s+Property\s+DefaultX\s*=\s*105\.0/.test(widgetSource));
    check('T11 papyrus DefaultY is 710', /float\s+Property\s+DefaultY\s*=\s*710\.0/.test(widgetSource));
    check('T11 AS default width is 96', /var\s+_width:Number\s*=\s*96\s*;/.test(meterSource));
    check('T11 AS default height is 4', /var\s+_height:Number\s*=\s*4\s*;/.test(meterSource));

    // The real mapping at the shipped default: on both aspect ratios (105, 710) must land the
    // whole 96x4 strip inside the safe span with the 3px margin respected, i.e. NOT clamped.
    const positionChecks = [
        { label: '16:9 identity', metrics: METRIC_CASES[0].metrics },
        { label: '16:10 logical frame', metrics: METRIC_CASES[1].metrics },
    ];
    for (const pc of positionChecks) {
        const meter = makeMeter(meterSource);
        meter._hudMetrics = pc.metrics;
        meter.setPositionX(105);
        meter.setPositionY(710);
        const expectedMapX = expectedX(pc.metrics, 105);
        const expectedMapY = expectedY(pc.metrics, 710);
        const clampMinX = -pc.metrics.hMin + 3;
        const clampMaxX = pc.metrics.hMax - 96 - 3;
        const clampMinY = -pc.metrics.vMin + 3;
        const clampMaxY = pc.metrics.vMax - 4 - 3;
        check('T11 ' + pc.label + ' default x follows the SkyUI mapping unclamped',
            near(meter._clip._x, expectedMapX) && expectedMapX >= clampMinX && expectedMapX <= clampMaxX,
            'got ' + meter._clip._x + ' expected ' + expectedMapX + ' span [' + clampMinX + ', ' + clampMaxX + ']');
        check('T11 ' + pc.label + ' default y follows the SkyUI mapping unclamped',
            near(meter._clip._y, expectedMapY) && expectedMapY >= clampMinY && expectedMapY <= clampMaxY,
            'got ' + meter._clip._y + ' expected ' + expectedMapY + ' span [' + clampMinY + ', ' + clampMaxY + ']');
    }

    // The clamp itself: push the strip past the safe span and the real updatePosition pulls it
    // back to the 3px margin, including the anchor offset the holder contributes.
    {
        const meter = makeMeter(meterSource);
        meter._hudMetrics = METRIC_CASES[0].metrics; // 0..1280 / 0..720
        meter.setPositionX(1279);
        check('T11 right overflow clamps to hMax - width - 3',
            near(meter._clip._x, 1280 - 96 - 3), 'got ' + meter._clip._x);
        meter.setPositionY(719);
        check('T11 bottom overflow clamps to vMax - height - 3',
            near(meter._clip._y, 720 - 4 - 3), 'got ' + meter._clip._y);
        meter.setPositionX(1);
        check('T11 left overflow clamps to hMin + 3', near(meter._clip._x, 3), 'got ' + meter._clip._x);

        // Center anchor shifts the holder by -width/2; the clamp bound must compensate for it.
        meter.setHAnchor('center');
        check('T11 center anchor offsets the holder', near(meter._holder._x, -48), 'got ' + meter._holder._x);
        meter.setPositionX(1);
        check('T11 left clamp compensates the holder offset',
            near(meter._clip._x, -meter._holder._x + 3), 'got ' + meter._clip._x);
    }
}

// T12 - setPercent redraws only when the 10% display step actually changes.
{
    const meter = makeMeter(meterSource);
    meter.createStage();
    check('T12 fresh stage draws nothing', meter._fill.ops.length === 0);

    drawAt(meter, 0.11); // display step 0.2
    const opsAfterFirst = meter._fill.ops.length;
    check('T12 first push in a step draws', opsAfterFirst > 0);

    meter.setPercent(0.15); // same display step 0.2
    check('T12 same-step push does not redraw', meter._fill.ops.length === opsAfterFirst,
        'got ' + meter._fill.ops.length + ' expected ' + opsAfterFirst);

    meter.setPercent(0.21); // display step 0.3
    check('T12 step change redraws', meter._fill.ops.length > opsAfterFirst);

    meter.setPercent(0.29); // still 0.3
    const opsAfterStep = meter._fill.ops.length;
    meter.setPercent(0.25);
    check('T12 same-step push again does not redraw', meter._fill.ops.length === opsAfterStep);

    // Raw percent is still kept even when no redraw happened.
    check('T12 raw percent kept across same-step pushes', near(meter._percent, 0.25, 1e-12),
        'got ' + meter._percent);
}

// T13 - setReady is a no-op while readiness does not change: no redraw, no pulse restart.
{
    const meter = makeMeter(meterSource);
    meter.createStage();
    drawAt(meter, 0.5);
    meter.setReady(true);
    const opsAfterGold = meter._fill.ops.length;
    const pulseFirst = meter._fill.onEnterFrame;
    check('T13 setReady(true) pulses', typeof pulseFirst === 'function');

    meter.setReady(true); // unchanged
    check('T13 unchanged setReady(true) does not redraw', meter._fill.ops.length === opsAfterGold,
        'got ' + meter._fill.ops.length + ' expected ' + opsAfterGold);
    check('T13 unchanged setReady(true) does not restart the pulse', meter._fill.onEnterFrame === pulseFirst);

    meter.setReady(false);
    check('T13 setReady(false) redraws and stops the pulse',
        meter._fill.ops.length > opsAfterGold && meter._fill.onEnterFrame === undefined);
}

// T14 - the beveled frame: the background shape extends 2px beyond the fill rect on every side
// and the bevel cuts adapt to the rectangle (no sharp top-left / bottom-right corners).
{
    const meter = makeMeter(meterSource);
    meter.createStage();
    meter._percent = 1; // full fill so the fill shape spans the whole strip
    meter.redraw();

    // Collect the polygon points a stage pass drew into a clip.
    const pointsOf = (ops) => {
        const points = [];
        for (const op of ops)
            if (op[0] === 'moveTo' || op[0] === 'lineTo')
                points.push([op[1], op[2]]);
        return points;
    };
    const bounds = (points) => ({
        minX: Math.min.apply(null, points.map((p) => p[0])),
        maxX: Math.max.apply(null, points.map((p) => p[0])),
        minY: Math.min.apply(null, points.map((p) => p[1])),
        maxY: Math.max.apply(null, points.map((p) => p[1])),
    });

    const bgPoints = pointsOf(meter._bg.ops);
    const bgOuter = bounds(bgPoints.slice(0, 5)); // first drawShape pass: the outer silhouette
    check('T14 frame extends 2px left of the fill', near(bgOuter.minX, -2), JSON.stringify(bgOuter));
    check('T14 frame extends 2px above the fill', near(bgOuter.minY, -2), JSON.stringify(bgOuter));
    check('T14 frame extends 2px right of the fill', near(bgOuter.maxX, meter._width + 2), JSON.stringify(bgOuter));
    check('T14 frame extends 2px below the fill', near(bgOuter.maxY, meter._height + 2), JSON.stringify(bgOuter));

    // Every fill vertex stays strictly inside the outer frame polygon (bounding-box formulation).
    const fillBounds = bounds(pointsOf(meter._fill.ops));
    check('T14 fill stays inside the frame horizontally',
        fillBounds.minX > bgOuter.minX && fillBounds.maxX < bgOuter.maxX, JSON.stringify(fillBounds));
    check('T14 fill stays inside the frame vertically',
        fillBounds.minY > bgOuter.minY && fillBounds.maxY < bgOuter.maxY, JSON.stringify(fillBounds));

    // The bevel: width 96, height 4 -> bevel = min(height, width/4) = 4, so the top-left corner
    // starts at (bevel, 0) and the bottom-right corner ends at (width - bevel, height): the
    // corners at (0,0) and (width, height) are cut, top-right and bottom-left stay sharp.
    const fillPoints = pointsOf(meter._fill.ops);
    const bevel = Math.min(meter._height, meter._width / 4);
    check('T14 bevel is min(height, width/4)', near(bevel, 4), 'got ' + bevel);
    check('T14 top-left corner is cut to the bevel',
        near(fillPoints[0][0], bevel) && near(fillPoints[0][1], 0), JSON.stringify(fillPoints[0]));
    check('T14 top-right corner stays sharp at (width, 0)',
        fillPoints.some((p) => near(p[0], meter._width) && near(p[1], 0)), JSON.stringify(fillPoints));
    check('T14 bottom-right corner is cut to the bevel',
        fillPoints.some((p) => near(p[0], meter._width - bevel) && near(p[1], meter._height)), JSON.stringify(fillPoints));
    check('T14 bottom-left corner stays sharp at (0, height)',
        fillPoints.some((p) => near(p[0], 0) && near(p[1], meter._height)), JSON.stringify(fillPoints));
    check('T14 shape closes back onto its first vertex',
        near(fillPoints[fillPoints.length - 1][0], fillPoints[0][0]) &&
        near(fillPoints[fillPoints.length - 1][1], fillPoints[0][1]), JSON.stringify(fillPoints));

    // The frame's own bevel adapts to its larger rectangle: the outer pass spans
    // (width+4) x (height+4), so min(height+4, (width+4)/4) governs its cut.
    const frameBevel = Math.min(meter._height + 4, (meter._width + 4) / 4);
    check('T14 frame bevel adapts to the frame rectangle', near(bgPoints[0][0], -2 + frameBevel),
        'got ' + bgPoints[0][0] + ' expected ' + (-2 + frameBevel));
}

/* ---------------------------------------------------------------------------------------------- */

if (failures.length) {
    console.error('FAIL ' + failures.length + ' check(s), ' + passed + ' passed\n');
    for (const failure of failures) console.error('  x ' + failure);
    process.exit(1);
}
console.log('PASS ' + passed + ' checks (offline AS2/Papyrus contract; no visual verification)');
