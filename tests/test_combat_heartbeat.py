# -*- coding: utf-8 -*-
"""Offline behaviour test for the KR_PlayerAlias single-shot heartbeat chain.

    python tests/test_combat_heartbeat.py

It does NOT re-implement the script: it reads papyrus/KR_PlayerAlias.psc, extracts the real
function bodies (EnsureBridge, ArmRageWatch, OnUpdate, OnPlayerLoadGame, OnRageChanged,
RegisterInputControls, RegisterRageEvents) and executes them in a tiny Papyrus-statement
interpreter with mock natives (KR_Native), a mock clock (Utility) and mock Config/Widget
partners. Only the constructs those functions use are supported: if/elseIf/else/endIf,
return, typed declarations, assignments and call expressions.

What this can prove: registration/arm sequencing, the lazy one-time init, the re-arm-every-tick
heartbeat (no permanent raging/pending gate), the _nextWatchAt stale-event debounce, the
single-shot timer discipline (RegisterForSingleUpdate only, no RegisterForUpdate/while/Wait)
and the load-game reset order. What it cannot prove: anything inside the native DLL or the
real Papyrus VM scheduler.
"""

import json
import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALIAS_PSC = os.path.join(ROOT, 'papyrus', 'KR_PlayerAlias.psc')
NATIVE_PSC = os.path.join(ROOT, 'papyrus', 'KR_Native.psc')

with open(ALIAS_PSC, 'r', encoding='utf-8') as handle:
    ALIAS_SOURCE = handle.read()
with open(NATIVE_PSC, 'r', encoding='utf-8') as handle:
    NATIVE_SOURCE = handle.read()


def strip_comments(text):
    lines = []
    for line in text.splitlines():
        line = line.split(';', 1)[0].rstrip()
        if line.strip():
            lines.append(line)
    return lines


def extract_bodies(source):
    """Map of name -> statement lines for every Function/Event block."""
    bodies = {}
    pattern = re.compile(r'^[ \t]*(Event|Function)[ \t]+([A-Za-z_]\w*)[ \t]*\(', re.M)
    for match in pattern.finditer(source):
        kind, name = match.group(1), match.group(2)
        terminator = 'EndEvent' if kind == 'Event' else 'EndFunction'
        rest = source[match.end():]
        # Skip the rest of the parameter list before the body starts.
        depth = 1
        i = 0
        while depth:
            if rest[i] == '(':
                depth += 1
            elif rest[i] == ')':
                depth -= 1
            i += 1
        rest = rest[i:]
        end = rest.index(terminator)
        bodies[name] = strip_comments(rest[:end])
    return bodies


FUNCTIONS = extract_bodies(ALIAS_SOURCE)
# Comment-free code, for structural assertions that must not trip over prose in comments.
ALIAS_CODE = '\n'.join(strip_comments(ALIAS_SOURCE))


class NativeMock(object):
    def __init__(self):
        self.available = False
        self.calls = []

    def _log(self, name):
        self.calls.append(name)

    def IsAvailable(self):
        return self.available

    def TickCombat(self):
        self._log('TickCombat')

    def ReconcileAfterLoad(self):
        self._log('ReconcileAfterLoad')

    def IsRaging(self):
        return True

    def IsPending(self):
        return True


class UtilityMock(object):
    def __init__(self):
        self.now = 0.0

    def GetCurrentRealTime(self):
        return self.now


class ConfigMock(object):
    GlowEnabled = True
    GlowDuration = 1.0

    def __init__(self):
        self.calls = []

    def LoadConfiguration(self):
        self.calls.append('LoadConfiguration')

    def HandlePlayerLoad(self):
        self.calls.append('HandlePlayerLoad')

    def AllowFullFeedback(self):
        return True


class WidgetMock(object):
    def __init__(self):
        self.calls = []

    def OnGameReload(self):
        self.calls.append('OnGameReload')

    def RefreshVisibility(self):
        self.calls.append('RefreshVisibility')


class AliasScript(object):
    """Executes the real KR_PlayerAlias function bodies against mocks."""

    SCRIPT_BUILTINS = (
        'UnregisterForUpdate', 'RegisterForSingleUpdate', 'UnregisterForModEvent',
        'RegisterForModEvent', 'UnregisterForControl', 'RegisterForControl',
    )

    def __init__(self):
        self.native = NativeMock()
        self.utility = UtilityMock()
        self.config = ConfigMock()
        self.widget = WidgetMock()
        self.calls = []  # every script-builtin call, in order: (name, args)
        self.state = {'_rageWatchArmed': False, '_watchInitialized': False, '_nextWatchAt': 0.0}
        self.properties = {}
        for match in re.finditer(r'^(?:float|bool|string|int)[ \t]+Property[ \t]+([A-Za-z_]\w*)'
                                 r'[ \t]*=[ \t]*(.+?)[ \t]+AutoReadOnly[ \t]*$', ALIAS_SOURCE, re.M):
            literal = match.group(2)
            if literal.startswith('"'):
                self.properties[match.group(1)] = json.loads(literal)
            else:
                self.properties[match.group(1)] = float(literal)

    # ---- recording builtins ----------------------------------------------------------------------

    def _builtin(self, name):
        def record(*args):
            self.calls.append((name,) + args)
        return record

    # ---- expression evaluation -------------------------------------------------------------------

    def eval_expr(self, expr, local):
        py = expr.replace('&&', ' and ').replace('||', ' or ')
        py = re.sub(r'(?<![!\w=<>])!(?!=)', ' not ', py)
        env = {}
        env.update(self.properties)
        env.update(self.state)
        env.update(local)
        env.update({'KR_Native': self.native, 'Utility': self.utility, 'Config': self.config,
                    'Widget': self.widget, 'Game': None})
        for name in self.SCRIPT_BUILTINS:
            env[name] = self._builtin(name)
        return eval(py, {'__builtins__': {}}, env)  # noqa: eval - restricted env, trusted source

    # ---- statement execution ---------------------------------------------------------------------

    def _find_block_end(self, lines, start):
        """Index of the endIf/else/elseIf closing the if at `start` (same nesting depth)."""
        depth = 0
        for i in range(start, len(lines)):
            stripped = lines[i].strip()
            if re.match(r'^if[ \t]*\(', stripped):
                depth += 1
            elif stripped == 'endIf':
                depth -= 1
                if depth == 0:
                    return i
            elif depth == 1 and (stripped == 'else' or re.match(r'^elseIf[ \t]*\(', stripped)):
                return i
        raise AssertionError('unterminated if block in:\n' + '\n'.join(lines))

    def exec_lines(self, lines, local=None):
        local = local if local is not None else {}
        i = 0
        while i < len(lines):
            line = lines[i].strip()
            if_match = re.match(r'^if[ \t]*\((.*)\)$', line)
            if if_match:
                end = self._find_block_end(lines, i)
                taken = self.eval_expr(if_match.group(1), local)
                branch_start, branch_end = i + 1, end
                branch_cond = True
                while True:
                    if taken and branch_cond:
                        result = self.exec_lines(lines[branch_start:branch_end], local)
                        if result == 'return':
                            return result
                    if branch_end == end and lines[end].strip() == 'endIf':
                        break
                    branch_cond = False
                    header = lines[branch_end].strip()
                    if header == 'else':
                        branch_start, branch_end = branch_end + 1, end
                        taken = True
                    else:
                        cond = re.match(r'^elseIf[ \t]*\((.*)\)$', header)
                        taken = self.eval_expr(cond.group(1), local)
                        branch_start = branch_end + 1
                        nxt = self._find_block_end(lines[branch_start:], 0) + branch_start
                        branch_end = nxt
                        if not taken:
                            continue
                    if branch_end == end and lines[end].strip() == 'endIf' and header == 'else':
                        result = self.exec_lines(lines[branch_start:branch_end], local)
                        if result == 'return':
                            return result
                        break
                i = end + 1
                continue

            if line == 'return' or line.startswith('return '):
                return 'return'

            whole_call = re.match(r'^([A-Za-z_]\w*)\(\)$', line)
            if whole_call and whole_call.group(1) in FUNCTIONS:
                self.run(whole_call.group(1))
                i += 1
                continue

            declared = re.match(r'^(?:float|bool|string|int|Form|Actor)[ \t]+([A-Za-z_]\w*)'
                                r'[ \t]*=[ \t]*(.+)$', line)
            assigned = re.match(r'^([A-Za-z_]\w*)[ \t]*=[ \t]*(.+)$', line)
            if declared:
                local[declared.group(1)] = self.eval_expr(declared.group(2), local)
            elif assigned:
                self.state[assigned.group(1)] = self.eval_expr(assigned.group(2), local)
            else:
                self.eval_expr(line, local)  # bare call statement
            i += 1
        return None

    def run(self, name):
        if name not in FUNCTIONS:
            raise AssertionError('function not found in KR_PlayerAlias.psc: ' + name)
        self.exec_lines(FUNCTIONS[name])

    # ---- helpers ---------------------------------------------------------------------------------

    def register_calls(self, name):
        return [c for c in self.calls if c[0] == name]

    def call_names(self):
        return [c[0] for c in self.calls]


class TestEnsureBridge(unittest.TestCase):
    def setUp(self):
        self.script = AliasScript()

    def test_unavailable_still_arms_the_watch(self):
        self.script.native.available = False
        self.script.run('EnsureBridge')
        arms = self.script.register_calls('RegisterForSingleUpdate')
        self.assertEqual(len(arms), 1)
        self.assertEqual(arms[0][1], self.script.properties['RageEndWatchInterval'])
        self.assertTrue(self.script.state['_rageWatchArmed'])
        self.assertEqual(self.script.register_calls('RegisterForControl'), [])
        self.assertEqual(self.script.register_calls('RegisterForModEvent'), [])

    def test_available_registers_input_and_rage_events(self):
        self.script.native.available = True
        self.script.run('EnsureBridge')
        self.assertEqual(self.script.register_calls('RegisterForControl'),
                         [('RegisterForControl', 'Right Attack/Block')])
        events = [c[1] for c in self.script.register_calls('RegisterForModEvent')]
        self.assertEqual(events, ['KR_RageBecameFull', 'KR_RageChanged'])
        # Unregister is issued right before each register: idempotent, exactly one registration.
        names = self.script.call_names()
        self.assertLess(names.index('UnregisterForControl'), names.index('RegisterForControl'))
        self.assertLess(names.index('UnregisterForModEvent'), names.index('RegisterForModEvent'))

    def test_arming_is_idempotent_while_armed(self):
        self.script.native.available = True
        self.script.run('EnsureBridge')
        self.script.run('EnsureBridge')
        self.assertEqual(len(self.script.register_calls('RegisterForSingleUpdate')), 1)


class TestOnUpdate(unittest.TestCase):
    def setUp(self):
        self.script = AliasScript()
        self.script.native.available = True

    def tick(self):
        self.script.utility.now += 10.0
        self.script.run('OnUpdate')

    def test_first_available_tick_initializes_once_and_ticks(self):
        self.tick()
        self.assertEqual(self.script.native.calls, ['TickCombat'])
        self.assertEqual(self.script.config.calls, ['LoadConfiguration'])
        self.assertEqual(self.script.register_calls('RegisterForControl'),
                         [('RegisterForControl', 'Right Attack/Block')])
        self.assertEqual([c[1] for c in self.script.register_calls('RegisterForModEvent')],
                         ['KR_RageBecameFull', 'KR_RageChanged'])
        self.assertTrue(self.script.state['_watchInitialized'])
        # The tick re-arms the single-shot chain: one timer planned per tick.
        self.assertEqual(len(self.script.register_calls('RegisterForSingleUpdate')), 1)
        self.assertTrue(self.script.state['_rageWatchArmed'])

    def test_second_tick_ticks_again_without_reinitializing(self):
        self.tick()
        self.tick()
        self.assertEqual(self.script.native.calls, ['TickCombat', 'TickCombat'])
        self.assertEqual(self.script.config.calls, ['LoadConfiguration'])  # still once
        self.assertEqual(len(self.script.register_calls('RegisterForControl')), 1)
        self.assertEqual(len(self.script.register_calls('RegisterForSingleUpdate')), 2)

    def test_no_permanent_raging_or_pending_gate(self):
        # The native reports raging AND pending; OnUpdate must still call TickCombat every tick.
        self.tick()
        self.tick()
        self.assertEqual(self.script.native.calls.count('TickCombat'), 2)
        # Structural: the OnUpdate body never consults rage/pending state as a gate.
        body = '\n'.join(FUNCTIONS['OnUpdate'])
        self.assertNotIn('IsRaging', body)
        self.assertNotIn('IsPending', body)

    def test_next_watch_at_debounces_stale_duplicate_events(self):
        self.script.run('ArmRageWatch')  # armed at t=0, deadline 0.5
        self.assertEqual(self.script.state['_nextWatchAt'], 0.5)
        self.script.utility.now = 0.2  # an old duplicate update fires early
        self.script.run('OnUpdate')
        self.assertEqual(self.script.native.calls, [])  # no TickCombat yet
        self.assertEqual(self.script.register_calls('RegisterForSingleUpdate'),
                         [('RegisterForSingleUpdate', 0.5),   # the original arm at t=0
                          ('RegisterForSingleUpdate', 0.3)])  # re-scheduled to the deadline
        self.assertTrue(self.script.state['_rageWatchArmed'])
        self.script.utility.now = 0.6  # deadline passed: the real tick happens
        self.script.run('OnUpdate')
        self.assertEqual(self.script.native.calls, ['TickCombat'])

    def test_unavailable_tick_still_rearms_and_skips_native(self):
        self.script.native.available = False
        self.tick()
        self.assertEqual(self.script.native.calls, [])
        self.assertEqual(len(self.script.register_calls('RegisterForSingleUpdate')), 1)
        self.assertEqual(self.script.config.calls, [])


class TestSingleShotDiscipline(unittest.TestCase):
    def test_source_never_uses_polling_or_blocking_primitives(self):
        self.assertNotIn('RegisterForUpdate(', ALIAS_CODE.replace('RegisterForSingleUpdate', ''))
        self.assertNotRegex(ALIAS_CODE, r'\bwhile\b')
        self.assertNotRegex(ALIAS_CODE, r'\bWait\(')

    def test_arm_uses_exactly_one_single_update_registration(self):
        script = AliasScript()
        script.run('ArmRageWatch')
        self.assertEqual(script.register_calls('RegisterForSingleUpdate'),
                         [('RegisterForSingleUpdate', script.properties['RageEndWatchInterval'])])

    def test_rage_changed_rearms_but_does_not_double_book(self):
        script = AliasScript()
        script.run('ArmRageWatch')
        script.run('OnRageChanged')  # native event while already armed
        self.assertEqual(len(script.register_calls('RegisterForSingleUpdate')), 1)


class TestOnPlayerLoadGame(unittest.TestCase):
    def setUp(self):
        self.script = AliasScript()
        self.script.native.available = True
        # A previous process left a timer and dirty flags behind.
        self.script.state['_rageWatchArmed'] = True
        self.script.state['_nextWatchAt'] = 123.4
        self.script.state['_watchInitialized'] = True

    def test_reset_order_clears_state_before_rearm(self):
        body = FUNCTIONS['OnPlayerLoadGame']
        text = '\n'.join(body)
        order = [text.index(marker) for marker in
                 ('UnregisterForUpdate()', '_rageWatchArmed = False', '_nextWatchAt = 0.0',
                  '_watchInitialized = False', 'EnsureBridge()')]
        self.assertEqual(order, sorted(order), 'reset must happen before EnsureBridge:\n' + text)

    def test_load_resets_then_rebuilds_the_chain(self):
        self.script.run('OnPlayerLoadGame')
        names = self.script.call_names()
        self.assertEqual(names[0], 'UnregisterForUpdate')  # saved deadlines die first
        # The reset happened (observable via the fresh re-arm EnsureBridge did afterwards).
        self.assertEqual(self.script.register_calls('RegisterForSingleUpdate'),
                         [('RegisterForSingleUpdate', self.script.properties['RageEndWatchInterval'])])
        self.assertTrue(self.script.state['_rageWatchArmed'])
        self.assertEqual(self.script.state['_nextWatchAt'], self.script.utility.now + 0.5)
        # Native anti-pause reconciliation runs only when the bridge is available.
        self.assertEqual(self.script.native.calls, ['ReconcileAfterLoad'])
        self.assertEqual(self.script.config.calls, ['HandlePlayerLoad'])
        self.assertEqual(self.script.widget.calls, ['OnGameReload', 'RefreshVisibility'])

    def test_load_without_native_still_resets_and_syncs_partners(self):
        self.script.native.available = False
        self.script.run('OnPlayerLoadGame')
        self.assertEqual(self.script.native.calls, [])
        self.assertEqual(self.script.config.calls, ['HandlePlayerLoad'])
        self.assertEqual(self.script.widget.calls, ['OnGameReload', 'RefreshVisibility'])
        self.assertEqual(len(self.script.register_calls('RegisterForSingleUpdate')), 1)

    def test_reconcile_is_guarded_by_availability_in_the_source(self):
        body = '\n'.join(FUNCTIONS['OnPlayerLoadGame'])
        guard = body.index('if (KR_Native.IsAvailable())')
        reconcile = body.index('KR_Native.ReconcileAfterLoad()')
        end_if = body.index('endIf', guard)
        self.assertLess(guard, reconcile)
        self.assertLess(reconcile, end_if)


class TestNativeContract(unittest.TestCase):
    """The alias may only call natives that KR_Native.psc actually declares."""

    def test_called_natives_are_declared_global_native(self):
        for name in ('IsAvailable', 'TickCombat', 'ReconcileAfterLoad'):
            self.assertRegex(NATIVE_SOURCE,
                             r'(?:bool\s+)?Function\s+' + name + r'\(\)\s*global\s+native')

    def test_alias_uses_no_undeclared_native_entry_points(self):
        called = set(re.findall(r'KR_Native\.([A-Za-z_]\w*)', ALIAS_SOURCE))
        declared = set(re.findall(r'Function\s+([A-Za-z_]\w*)\(', NATIVE_SOURCE))
        self.assertTrue(called)
        self.assertEqual(called - declared, set())


if __name__ == '__main__':
    unittest.main(verbosity=2)
