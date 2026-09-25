#!/usr/bin/env python3
"""Integration tests for the KratosRagePatch staging folder.

Run with:  python tests/test_package.py

Everything is checked against the artifacts on disk:
  * the ESP/SEQ are parsed here as raw binaries (independent of Mutagen) and the deep structural
    report produced by tools/PluginBuilder is required to be all-green;
  * the OAR configs are re-checked against the OpenAnimationReplacer 2.3.4 rules the generator
    claims to follow (see tools/generate_oar.py): one <source>Ready submod per equipment set that
    keeps its source's conditions verbatim and only appends the patch's 0x803 gate, overlaid
    configs that disable exactly the agreed animations, and Trigger2 untouched;
  * the PayloadInterpreter ini is rebuilt from the original file with a second, independent
    implementation of the rewrite rule and compared byte for byte;
  * data/integration.json is cross-checked against build/PluginBuilder/record-report.json, and the
    curated scoring whitelist is checked to be a complete, disjoint partition of the analyzer's
    candidates with buffs/dust/barehand-swap forms outside it.

Checks that need the original mod / the game install are skipped - never failed - when those paths
are absent, so the packaged stage can be validated on any machine.
"""

from __future__ import annotations

import json
import re
import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
STAGE = REPO / "build" / "stage"
PLUGIN = STAGE / "KratosRagePatch.esp"
SEQ = STAGE / "Seq" / "KratosRagePatch.seq"
BUILD_REPORT = REPO / "build" / "PluginBuilder" / "build-report.json"
RECORD_REPORT = REPO / "build" / "PluginBuilder" / "record-report.json"
OAR_REPORT = REPO / "build" / "PluginBuilder" / "oar-report.json"
INTEGRATION = REPO / "data" / "integration.json"
GAMEBRIDGE = REPO / "src" / "GameBridge.cpp"
PAPYRUS_BINDINGS = REPO / "src" / "PapyrusBindings.cpp"
KR_NATIVE_PSC = REPO / "papyrus" / "KR_Native.psc"

OAR_REL = Path("meshes/actors/character/animations/OpenAnimationReplacer/KratosMoveset")
REQUEST_PAYLOAD = b"$SIBADA = @CAST|0x804|KratosRagePatch.esp|1|1|1|0|0|0|0|0|0"
# payloads that are castable and therefore kept verbatim (PI @CAST == CastSpellImmediate)
KEPT_FORM_TOKENS = ("0X817", "0X844")
# the only payload whose form is not a spell and can therefore never be applied
DROPPED_FORM_TOKENS = ("0X201DF97",)

# records fixed by docs/implementation-contracts.md
EXPECTED_RECORDS = {
    0x800: ("QUST", "KR_MainQuest"),
    0x801: ("QUST", "KR_WidgetQuest"),
    0x802: ("SPEL", "KR_RageReadyAbility"),
    0x803: ("MGEF", "KR_RageReadyEffect"),
    0x804: ("SPEL", "KR_ActivationRequestSpell"),
    0x805: ("MGEF", "KR_ActivationRequestEffect"),
    0x806: ("EFSH", "KR_FullRageGlow"),
}
EXPECTED_SCRIPTS = ["KR_Config", "KR_PlayerAlias", "KR_Widget"]

# source submod -> the animations the patch disables inside it
EXPECTED_DISABLES = {
    "SpartanRageTrigger": [f"mco_attack{i}.hkx" for i in range(1, 11)],
    "Barehanded": ["mco_weaponart.HKX"],
    "LeviathanAxe": ["mco_weaponart.HKX"],
}
READY_SUFFIX = "Ready"
READY_SOURCES = tuple(EXPECTED_DISABLES)
OAR_PROJECTS = {"DefaultMale", "DefaultFemale"}
UNTOUCHED_SUBMOD = "SpartanRageTrigger2"

# forms that must never appear in the curated scoring whitelist
BUFFS_AND_SWAP = {"0x817", "0x82C", "0x844", "0x847", "0x848", "0x86F"}
DUST = {"0x872", "0x874", "0x878"}
PERSISTENT_AREA = {"0x843"}


def load_json(path: Path):
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


# --------------------------------------------------------------------------- esp binary

def parse_plugin(data: bytes):
    """Minimal Skyrim plugin reader: masters, flags, form version and (signature, id, edid)."""
    if data[:4] != b"TES4":
        raise AssertionError("not a plugin: missing TES4 header")
    header_size = struct.unpack_from("<I", data, 4)[0]
    flags = struct.unpack_from("<I", data, 8)[0]
    # SSE record header: signature(4) size(4) flags(4) formID(4) unk(4) formVersion(2) unk(2)
    form_version = struct.unpack_from("<H", data, 20)[0]
    masters = []
    pos, end = 24, 24 + header_size
    while pos + 6 <= end:
        signature = data[pos:pos + 4]
        size = struct.unpack_from("<H", data, pos + 4)[0]
        payload = data[pos + 6:pos + 6 + size]
        if signature == b"MAST":
            masters.append(payload.split(b"\x00")[0].decode("ascii"))
        pos += 6 + size

    records = {}
    pos = end
    while pos + 24 <= len(data):
        signature = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        if signature != b"GRUP":
            break
        child, child_end = pos + 24, pos + size
        while child + 24 <= child_end:
            rec_sig = data[child:child + 4]
            rec_size = struct.unpack_from("<I", data, child + 4)[0]
            form_id = struct.unpack_from("<I", data, child + 12)[0]
            body = data[child + 24:child + 24 + rec_size]
            edid, off = None, 0
            while off + 6 <= len(body):
                sub_sig = body[off:off + 4]
                sub_size = struct.unpack_from("<H", body, off + 4)[0]
                if sub_sig == b"EDID":
                    edid = body[off + 6:off + 6 + sub_size].split(b"\x00")[0].decode("ascii")
                    break
                off += 6 + sub_size
            records[form_id & 0xFFFFFF] = (rec_sig.decode("ascii"), edid)
            child += 24 + rec_size
        pos = child_end
    return masters, flags, form_version, records


class TestPluginArtifact(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = PLUGIN.read_bytes()
        cls.masters, cls.flags, cls.form_version, cls.records = parse_plugin(cls.data)

    def test_esl_flagged_form_version_44(self):
        self.assertTrue(self.flags & 0x200, "ESL (Small) flag must be set")
        self.assertEqual(self.form_version, 44)

    def test_masters(self):
        self.assertEqual(sorted(self.masters), ["Dragonborn.esm", "Skyrim.esm"])

    def test_contract_records_present_with_expected_signature_and_editor_id(self):
        for form_id, (signature, editor_id) in EXPECTED_RECORDS.items():
            self.assertIn(form_id, self.records, f"0x{form_id:X} missing from the plugin")
            actual_signature, actual_editor_id = self.records[form_id]
            self.assertEqual(actual_signature, signature, f"0x{form_id:X}")
            self.assertEqual(actual_editor_id, editor_id, f"0x{form_id:X}")

    def test_no_record_outside_the_light_range(self):
        for form_id in self.records:
            self.assertGreaterEqual(form_id, 0x800)
            self.assertLessEqual(form_id, 0xFFF)

    def test_seq_lists_both_start_game_quests(self):
        seq = SEQ.read_bytes()
        self.assertEqual(len(seq) % 4, 0)
        values = [struct.unpack_from("<I", seq, i)[0] for i in range(0, len(seq), 4)]
        index = len(self.masters)
        self.assertEqual(values, [(index << 24) | 0x800, (index << 24) | 0x801])

    def test_plugin_builder_report_is_green(self):
        report = load_json(BUILD_REPORT)
        verify = report["verify"]
        failed = [c for c in verify["checks"] if not c["passed"]]
        self.assertEqual(failed, [], f"{len(failed)} verifier check(s) failed: "
                                     f"{[c['check'] for c in failed]}")
        self.assertTrue(verify["passed"])
        self.assertEqual(report["formVersion"], 44)
        self.assertTrue(report["eslFlagged"])

    def test_vmad_scripts_ship_as_compiled_pex(self):
        for name in EXPECTED_SCRIPTS:
            pex = STAGE / "Scripts" / f"{name}.pex"
            self.assertTrue(pex.is_file(), f"missing {pex}")
            self.assertGreater(pex.stat().st_size, 0)


def plugin_records(data: bytes):
    """local form id -> (signature, raw body), read straight out of the plugin binary."""
    header_size = struct.unpack_from("<I", data, 4)[0]
    records, pos = {}, 24 + header_size
    while pos + 24 <= len(data):
        if data[pos:pos + 4] != b"GRUP":
            break
        size = struct.unpack_from("<I", data, pos + 4)[0]
        child, child_end = pos + 24, pos + size
        while child + 24 <= child_end:
            signature = data[child:child + 4]
            rec_size = struct.unpack_from("<I", data, child + 4)[0]
            form_id = struct.unpack_from("<I", data, child + 12)[0]
            records[form_id & 0xFFFFFF] = (signature, data[child + 24:child + 24 + rec_size])
            child += 24 + rec_size
        pos = child_end
    return records


def subrecords(body: bytes):
    """(signature, payload) pairs of a record body."""
    out, off = [], 0
    while off + 6 <= len(body):
        signature = body[off:off + 4]
        size = struct.unpack_from("<H", body, off + 4)[0]
        out.append((signature, body[off + 6:off + 6 + size]))
        off += 6 + size
    return out


def read_script_entry(data: bytes, offset: int):
    """One VMAD script entry: name, flags and its object properties.

    The object union of an object property at object format 2 is 8 bytes laid out as
    [static: int16][alias: int16][form id: uint32]. This order is not guessed: in Skyrim.esm's
    jail quest (0x10D9F4) the 12 Alias_* object properties only decode to 12 distinct, matching
    alias indices under this order (Alias_Player -> 15, which is also the alias the
    JailQuestPlayerScript entry is attached to), while the other order collapses all of them to
    alias 0. The same order is what makes all 5910 form-valued vanilla object properties
    (`01 <flags> 00 00 ff ff <form id>`) mean "alias -1, use the form id".
    """
    name_len = struct.unpack_from("<H", data, offset)[0]
    offset += 2
    name = data[offset:offset + name_len].decode("ascii")
    offset += name_len
    flags = data[offset]
    offset += 1
    prop_count = struct.unpack_from("<h", data, offset)[0]
    offset += 2

    props = []
    for _ in range(prop_count):
        prop_len = struct.unpack_from("<H", data, offset)[0]
        offset += 2
        prop_name = data[offset:offset + prop_len].decode("ascii")
        offset += prop_len
        prop_type = data[offset]
        prop_flags = data[offset + 1]
        offset += 2
        assert prop_type == 1, f"{name}.{prop_name}: only Object properties exist in this plugin"
        raw = data[offset:offset + 8]
        offset += 8
        static, alias, form_id = struct.unpack_from("<hhI", raw, 0)
        props.append((prop_name, prop_flags, static, alias, form_id, raw))
    return (name, flags, props), offset


def read_script_entries(data: bytes, offset: int):
    """(entries, end offset) for a VMAD script list whose count sits at `offset`."""
    count = struct.unpack_from("<h", data, offset)[0]
    offset += 2
    entries = []
    for _ in range(count):
        entry, offset = read_script_entry(data, offset)
        entries.append(entry)
    return entries, offset


class TestVmadBindingsFromRawBytes(unittest.TestCase):
    """Independent (non-Mutagen) re-read of the VMAD the plugin builder wrote.

    Mutagen reading back its own file only proves self-consistency, so the bindings are decoded a
    second time here straight from the raw VMAD bytes, and the encodings are checked against the
    conventions of the shipped masters (object format 2 order, alias -1 for form values, the owning
    quest's own FormID in an alias entry).
    """

    @classmethod
    def setUpClass(cls):
        cls.data = PLUGIN.read_bytes()
        cls.records = plugin_records(cls.data)
        cls.masters, _, _, _ = parse_plugin(cls.data)
        cls.patch_index = len(cls.masters)

    def full(self, local: int) -> int:
        """The on-disk form id of one of the plugin's own records (the plugin is the last master)."""
        return (self.patch_index << 24) | local

    def vmad(self, local: int) -> bytes:
        payloads = [payload for signature, payload in subrecords(self.records[local][1])
                    if signature == b"VMAD"]
        self.assertEqual(len(payloads), 1, f"0x{local:X} must have exactly one VMAD")
        return payloads[0]

    def object_properties(self, vmad: bytes):
        version, object_format, script_count = struct.unpack_from("<hhh", vmad, 0)
        entries, offset = read_script_entries(vmad, 4)
        return (version, object_format, script_count), entries, offset

    # -------------------------------------------------------------- object format 2

    def test_object_union_layout_and_kr_config_widget(self):
        vmad = self.vmad(0x800)
        (version, object_format, script_count), entries, _ = self.object_properties(vmad)
        self.assertEqual((version, object_format, script_count), (5, 2, 1))
        self.assertEqual([e[0] for e in entries], ["KR_Config"])
        props = entries[0][2]
        self.assertEqual([p[0] for p in props], ["Widget"])
        name, flags, static, alias, form_id, raw = props[0]
        self.assertEqual(name, "Widget")
        self.assertEqual(alias, -1, "a form-valued property must carry alias -1")
        self.assertEqual(form_id, self.full(0x801))
        self.assertEqual(raw.hex(), "0000ffff" + struct.pack("<I", self.full(0x801)).hex())

    def test_alias_entry_binds_alias_zero_of_its_own_quest(self):
        vmad = self.vmad(0x800)
        script_name = b"KR_PlayerAlias"
        start = vmad.index(script_name) - 2
        version, object_format, script_count = struct.unpack_from("<hhh", vmad, start - 6)
        self.assertEqual((version, object_format, script_count), (5, 2, 1))
        entry_name_len = struct.unpack_from("<H", vmad, start - 14)[0]
        alias = struct.unpack_from("<h", vmad, start - 12)[0]
        owner = struct.unpack_from("<I", vmad, start - 10)[0]
        self.assertEqual(entry_name_len, 0)
        self.assertEqual(alias, 0, "KR_PlayerAlias must be attached to alias 0")
        self.assertEqual(owner, self.full(0x800),
                         "every one of Skyrim.esm's 2075 alias entries names its own quest here")
        entries, end = read_script_entries(vmad, start - 2)
        self.assertEqual([e[0] for e in entries], ["KR_PlayerAlias"])
        self.assertEqual(end, len(vmad), "the alias entry must be the last thing in the VMAD")

    def test_alias_script_binds_every_contract_form_with_alias_minus_one(self):
        vmad = self.vmad(0x800)
        start = vmad.index(b"KR_PlayerAlias") - 2
        entries, _ = read_script_entries(vmad, start - 2)
        props = {p[0]: p for p in entries[0][2]}
        self.assertEqual(sorted(props), ["Config", "FullGlow", "Widget"])
        for name, target in (("Config", 0x800), ("Widget", 0x801), ("FullGlow", 0x806)):
            prop_name, flags, static, alias, form_id, raw = props[name]
            self.assertEqual(alias, -1, f"{name}: a form value must carry alias -1, not 0")
            self.assertEqual(form_id, self.full(target), name)
            self.assertEqual(raw.hex(), "0000ffff" + struct.pack("<I", self.full(target)).hex(), name)

    def test_widget_quest_script_carries_no_property(self):
        vmad = self.vmad(0x801)
        (version, object_format, script_count), entries, offset = self.object_properties(vmad)
        self.assertEqual((version, object_format, script_count), (5, 2, 1))
        self.assertEqual([e[0] for e in entries], ["KR_Widget"])
        self.assertEqual(entries[0][2], [], "SKI_WidgetBase needs no VMAD property")
        self.assertEqual(offset, 20)

    def test_no_object_property_anywhere_keeps_a_stale_alias_index(self):
        # regression guard for the two encodings fixed here: a form-valued property must never be
        # written as "alias N of the form in the id field" (N == 0 in the broken version).
        for local in (0x800, 0x801):
            for _name, _flags, _static, alias, form_id, raw in self.every_object_property(self.vmad(local)):
                self.assertEqual(alias, -1, f"0x{local:X}: {raw.hex()} must be a form value")

    def every_object_property(self, vmad: bytes):
        _, entries, _ = self.object_properties(vmad)
        for _name, _flags, props in entries:
            yield from props
        if b"KR_PlayerAlias" in vmad:
            start = vmad.index(b"KR_PlayerAlias") - 2
            alias_entries, _ = read_script_entries(vmad, start - 2)
            for _name, _flags, props in alias_entries:
                yield from props


class TestNativeCrossFileConsistency(unittest.TestCase):
    """Keep the diagnostic scoring entry and activation tables consistent with the package."""

    @classmethod
    def setUpClass(cls):
        if not GAMEBRIDGE.is_file():
            cls.text = None
            return
        cls.text = GAMEBRIDGE.read_text(encoding="utf-8")
        cls.integration = load_json(INTEGRATION)
        cls.records = plugin_records(PLUGIN.read_bytes())

    def native(self):
        if self.text is None:
            self.skipTest(f"native source not present: {GAMEBRIDGE}")
        return self.text

    @staticmethod
    def ids(blob):
        return {int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]+)u?", blob)}

    def success_casts(self):
        text = self.native()
        self.assertIn("successCasts_.clear();", text)
        body = text.split("successCasts_.clear();", 1)[1].split("}) {", 1)[0]
        self.assertIn("initializer_list", body)
        return self.ids(body)

    def named_forms(self):
        text = self.native()
        named = {}
        for member in ("rageEffect_", "rageSpell_", "readyAbility_", "readyEffect_", "requestEffect_"):
            match = re.search(rf"{member}\s*=\s*Lookup<[^>]+>\(\s*0x([0-9A-Fa-f]+)\s*,", text)
            self.assertIsNotNone(match, f"{member} resolution not found")
            named[member] = int(match.group(1), 16)
        return named

    def spell_effects(self, local: int):
        return [struct.unpack_from("<I", payload, 0)[0] & 0xFFFFFF
                for signature, payload in subrecords(self.records[local][1])
                if signature == b"EFID"]

    def payloads(self, action):
        return [entry for entry in self.integration["originalPayloads"]
                if entry["patchAction"] == action]

    @staticmethod
    def local_id(requested_form):
        return int(requested_form.rsplit("x", 1)[1], 16)

    def test_combat_timer_is_the_only_production_gain_source(self):
        text = self.native()
        self.assertNotIn("specialSources_", text)
        self.assertNotIn("IsEligibleOpponent", text)
        self.assertNotIn("state_.TryAward(", text)
        self.assertNotIn("state_.TryAwardPlayerDamage(", text)
        self.assertIn("state_.UpdateCombat(", text)
        self.assertIn("void GameBridge::TickCombat()", text)
        self.assertIn("IsInCombat()", text)
        for legacy in ("SubmitPlayerDamage", "SubmitWeaponHit", "SubmitMagicHit", "SubmitProjectileHit",
                       "SubmitIncomingHit", "SubmitKill"):
            match = re.search(rf"void GameBridge::{legacy}\([^)]*\)\s*\{{([^}}]*)\}}", text)
            self.assertIsNotNone(match, f"missing legacy ABI entry: {legacy}")
            body = re.sub(r"//[^\n]*", "", match.group(1)).strip()
            self.assertEqual(body, "", f"{legacy} must not award or queue stale hits")
        alias = (REPO / "papyrus/KR_PlayerAlias.psc").read_text(encoding="utf-8")
        self.assertNotIn("PO3_Events_Alias.", alias)
        self.assertNotIn("InstallDamageHooks", (REPO / "src/Plugin.cpp").read_text(encoding="utf-8"))
        self.assertNotIn("src/DamageHooks.cpp", (REPO / "CMakeLists.txt").read_text(encoding="utf-8"))
        self.assertIn("KR_Native.TickCombat()", alias)

    def test_pi_kept_payloads_are_never_cast_again_by_the_native_success_casts(self):
        kept = {self.local_id(entry["requestedForm"]) for entry in self.payloads("keptVerbatim")}
        self.assertEqual(kept, {0x817, 0x844})
        casts = self.success_casts()
        self.assertEqual(casts & kept, set(),
                         "PI already casts these; the native side must not repeat them")

    def test_native_casts_exactly_the_payloads_pi_gave_up(self):
        replaced = {self.local_id(entry["requestedForm"])
                    for entry in self.payloads("replacedByRequestCast")}
        self.assertEqual(len(replaced), 4)
        named = self.named_forms()
        rage_spell = named["rageSpell_"]
        self.assertIn(rage_spell, replaced)
        self.assertEqual(self.success_casts(), replaced - {rage_spell})
        self.assertIn("CastLikePayload({rageSpell_", self.native(),
                      "the replaced rage spell must be cast by the activation transaction")

    def test_the_dropped_payload_is_not_cast_by_anybody(self):
        dropped = {self.local_id(entry["requestedForm"])
                   for entry in self.payloads("notAppliedNotCastable")}
        self.assertEqual(dropped, {0x1DF97})
        self.assertTrue(dropped.isdisjoint(self.success_casts()))
        pi = load_json(OAR_REPORT)["payloadInterpreter"]
        tokens = {form_token(line.encode()) for line in pi["keptPayloads"] + pi["replacedPayloads"]}
        self.assertTrue({"0X1DF97", "0X201DF97"}.isdisjoint(tokens))

    def test_activation_request_chain_agrees_across_pi_esp_and_native(self):
        report = load_json(OAR_REPORT)["payloadInterpreter"]
        request_token = form_token(report["requestPayload"].encode())
        self.assertEqual(request_token, "0x804")
        named = self.named_forms()
        self.assertEqual(named["requestEffect_"], 0x805)
        self.assertEqual(named["readyAbility_"], 0x802)
        self.assertEqual(named["readyEffect_"], 0x803)
        # the ESP spell must actually apply the magic effect the native listens for
        self.assertEqual(self.spell_effects(0x804), [0x805])
        self.assertEqual(self.spell_effects(0x802), [0x803])
        # and the OAR ready groups must gate on the same marker the native resolves
        gate = load_json(OAR_REPORT)["oarNewSubmods"][0]["appendedCondition"]["Magic effect"]["formID"]
        self.assertEqual(int(gate, 16), named["readyEffect_"])

    def test_staged_ini_keeps_only_the_payloads_the_native_does_not_cast(self):
        pi = load_json(OAR_REPORT)["payloadInterpreter"]
        kept = {form_token(line.encode()) for line in pi["keptPayloads"]}
        self.assertEqual(kept, {"0X817", "0X844"})
        replaced = {form_token(line.encode()) for line in pi["replacedPayloads"]}
        self.assertEqual(replaced, {"0X2FFD2", "0X814", "0X841", "0X7A82B"})

    def test_papyrus_native_declarations_match_the_bound_functions(self):
        """KR_Native.psc and PapyrusBindings.cpp must declare/bind the same global function set.

        Deliberately a set comparison, not a fixed count: the bridge is still being extended (the
        pending-state getter is being added), and what must never drift is that both sides name the
        same functions. A declared-but-unbound name is a hard runtime failure for its callers, and a
        bound-but-undeclared name means the DLL exposes something no script declares.
        """
        if not (PAPYRUS_BINDINGS.is_file() and KR_NATIVE_PSC.is_file()):
            self.skipTest("native sources not present")
        declared = set(re.findall(
            r"^\s*(?:Function|int Function|bool Function|float Function)\s+(\w+)\s*\(",
            KR_NATIVE_PSC.read_text(encoding="utf-8"), re.M))
        bound = set(re.findall(r'vm->RegisterFunction\("(\w+)"',
                               PAPYRUS_BINDINGS.read_text(encoding="utf-8")))
        self.assertEqual(
            (sorted(declared - bound), sorted(bound - declared)), ([], []),
            "declared-but-unbound / bound-but-undeclared: KR_Native.psc vs PapyrusBindings.cpp")
        # every native a Papyrus script actually calls must be declared and bound
        routed = set(re.findall(r"\bKR_Native\.(\w+)\s*\(",
                                "".join(p.read_text(encoding="utf-8")
                                        for p in sorted((REPO / "papyrus").glob("KR_*.psc")))))
        self.assertLessEqual(routed, declared, f"callers use undeclared natives: {routed - declared}")
        self.assertLessEqual(routed, bound, f"callers use unbound natives: {routed - bound}")


class TestOarArtifacts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.report = load_json(OAR_REPORT)
        cls.oar_root = STAGE / OAR_REL
        cls.mod_root = Path(cls.report["originalMod"])
        cls.source_oar = (cls.mod_root / "meshes" / "actors" / "character" / "animations"
                          / "OpenAnimationReplacer" / "KratosMoveset")

    def source_available(self):
        if not self.source_oar.is_dir():
            self.skipTest(f"original mod not present: {self.source_oar}")
        return self.source_oar

    def test_staged_files_are_exactly_the_expected_configs(self):
        staged = sorted(str(p.relative_to(self.oar_root)).replace("\\", "/")
                        for p in self.oar_root.rglob("*") if p.is_file())
        expected = sorted([f"{name}/config.json"
                           for name in list(EXPECTED_DISABLES)
                           + [f"{s}{READY_SUFFIX}" for s in READY_SOURCES]])
        self.assertEqual(staged, expected)

    def test_no_hkx_is_copied_into_the_patch(self):
        copied = [p for p in STAGE.rglob("*.hkx")] + [p for p in STAGE.rglob("*.HKX")]
        self.assertEqual([str(p) for p in copied], [], "animations must be reused, not copied")

    def test_trigger2_is_not_touched(self):
        self.assertFalse((self.oar_root / UNTOUCHED_SUBMOD).exists())
        self.assertEqual(self.report["oarUntouchedSubmods"], [UNTOUCHED_SUBMOD])

    def test_every_config_is_valid_oar_json(self):
        for path in self.oar_root.rglob("config.json"):
            document = load_json(path)
            self.assertIsInstance(document.get("name"), str, path)
            self.assertIsInstance(document.get("priority"), int, path)
            if "overrideAnimationsFolder" in document:
                self.assertIsInstance(document["overrideAnimationsFolder"], str, path)
            self.assertIsInstance(document.get("conditions", []), list, path)
            for entry in document.get("replacementAnimDatas", []):
                self.assertIsInstance(entry.get("projectName"), str, path)
                self.assertIsInstance(entry.get("path"), str, path)
                self.assertTrue(entry["path"].startswith("data\\meshes\\"), path)

    def test_one_ready_group_per_equipment_set_that_reuses_its_source_folder(self):
        self.assertIsInstance(self.report["oarNewSubmods"], list)
        self.assertEqual([r["submod"] for r in self.report["oarNewSubmods"]],
                         [f"{s}{READY_SUFFIX}" for s in READY_SOURCES])
        for source in READY_SOURCES:
            ready = load_json(self.oar_root / f"{source}{READY_SUFFIX}" / "config.json")
            folder = self.oar_root / f"{source}{READY_SUFFIX}"
            self.assertEqual(ready["name"], f"{source}{READY_SUFFIX}")
            self.assertEqual(ready["overrideAnimationsFolder"], source)
            self.assertEqual(sorted(p.name for p in folder.iterdir()), ["config.json"],
                             f"{folder} must not contain animations of its own")
            self.assertFalse(ready.get("replacementAnimDatas"))

    def test_ready_group_keeps_every_original_condition_and_only_appends_the_gate(self):
        source_root = self.source_available()
        for source in READY_SOURCES:
            ready = load_json(self.oar_root / f"{source}{READY_SUFFIX}" / "config.json")
            original = load_json(source_root / source / "config.json")
            self.assertEqual(ready["conditions"][:-1], original["conditions"], source)
            self.assertEqual(len(ready["conditions"]), len(original["conditions"]) + 1, source)
            gate = ready["conditions"][-1]
            self.assertEqual(gate["condition"], "HasMagicEffect", source)
            self.assertEqual(gate["Magic effect"]["pluginName"], "KratosRagePatch.esp", source)
            self.assertEqual(gate["Magic effect"]["formID"], "803", source)
            self.assertFalse(gate.get("negated"), "the ready gate must not be negated")
            # the Keytrace and equipment conditions of the source must survive verbatim
            self.assertEqual(
                [c for c in ready["conditions"] if "Keytrace" in json.dumps(c)],
                [c for c in original["conditions"] if "Keytrace" in json.dumps(c)],
                source)
            self.assertEqual(
                [c for c in ready["conditions"] if "IsEquipped" in json.dumps(c)],
                [c for c in original["conditions"] if "IsEquipped" in json.dumps(c)],
                source)

    def test_ready_group_keeps_the_source_priority(self):
        source_root = self.source_available()
        for source in READY_SOURCES:
            ready = load_json(self.oar_root / f"{source}{READY_SUFFIX}" / "config.json")
            original = load_json(source_root / source / "config.json")
            self.assertEqual(ready["priority"], original["priority"], source)
        reported = {r["submod"]: r for r in self.report["oarNewSubmods"]}
        for source in READY_SOURCES:
            self.assertEqual(reported[f"{source}{READY_SUFFIX}"]["prioritySource"],
                             f"{source}/config.json")

    def test_overlays_disable_exactly_the_expected_animations(self):
        for submod, files in EXPECTED_DISABLES.items():
            document = load_json(self.oar_root / submod / "config.json")
            disabled = [e for e in document.get("replacementAnimDatas", []) if e.get("disabled")]
            self.assertTrue(disabled, submod)
            self.assertEqual(sorted({Path(e["path"]).name for e in disabled}), sorted(files),
                             submod)
            self.assertEqual({e["projectName"] for e in disabled}, OAR_PROJECTS, submod)
            self.assertEqual(len(disabled), len(files) * len(OAR_PROJECTS), submod)
            for entry in disabled:
                self.assertEqual(
                    entry["path"],
                    "data\\meshes\\actors\\character\\animations\\OpenAnimationReplacer"
                    f"\\KratosMoveset\\{submod}\\" + Path(entry["path"]).name,
                    submod)

    def test_overlays_keep_the_original_conditions_and_priority(self):
        source_root = self.source_available()
        for submod in EXPECTED_DISABLES:
            patched = load_json(self.oar_root / submod / "config.json")
            original = load_json(source_root / submod / "config.json")
            self.assertEqual(patched["conditions"], original["conditions"], submod)
            self.assertEqual(patched["priority"], original["priority"], submod)
            self.assertEqual(patched["name"], original["name"], submod)
            self.assertEqual(set(patched) - {"replacementAnimDatas"},
                             set(original) - {"replacementAnimDatas"}, submod)

    def test_report_matches_the_staged_files_and_records_the_format_evidence(self):
        for overlay in self.report["oarOverlays"]:
            self.assertEqual(Path(overlay["target"]).read_bytes(),
                             (self.oar_root / overlay["submod"] / "config.json").read_bytes())
            self.assertEqual(overlay["disabledAnimations"],
                             [Path(n).stem for n in EXPECTED_DISABLES[overlay["submod"]]])
        for ready in self.report["oarNewSubmods"]:
            self.assertEqual(Path(ready["target"]).read_bytes(),
                             (self.oar_root / ready["submod"] / "config.json").read_bytes())
            self.assertEqual(ready["hkxCopied"], 0)
        evidence = " ".join(self.report["oarFormatEvidence"])
        for needle in ("Parsing.h:106", "Parsing.cpp:749-761", "UIMain.cpp:1141-1162",
                       "ReplacementAnimation.cpp:279"):
            self.assertIn(needle, evidence)


# --------------------------------------------------------------------------- payload interpreter

def form_token(line: bytes) -> str:
    """Independent extraction of the form token of a payload line."""
    match = re.match(rb"^\$SIBADA\s*=\s*@\w+\|([^|]+)\|", line.strip())
    return match.group(1).decode("ascii") if match else ""


def rebuild_sibada_block(lines, request, keep_tokens):
    """Second implementation of the rewrite: request cast + kept castable payloads, rest dropped."""
    active = [i for i, line in enumerate(lines)
              if line.lstrip().startswith(b"$SIBADA") and not line.lstrip().startswith(b";")]
    keep = [i for i in active if form_token(lines[i]) in keep_tokens]
    out, first = [], True
    for index, line in enumerate(lines):
        if index in active:
            if first:
                out.append(request)
                out.extend(lines[i] for i in keep)
                first = False
            continue
        out.append(line)
    return out, active, keep


class TestPayloadInterpreter(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.report = load_json(OAR_REPORT)
        cls.pi = cls.report["payloadInterpreter"]
        cls.stage_ini = Path(cls.pi["target"])
        cls.original = Path(cls.pi["sourceIni"])

    def test_ini_is_staged_under_the_vfs_path_of_the_original(self):
        self.assertEqual(self.stage_ini.name, "God of War4.ini")
        self.assertEqual(self.stage_ini.parent.name, "Config")
        self.assertEqual(self.stage_ini.parent.parent.name, "PayloadInterpreter")

    def test_active_payloads_are_the_request_cast_plus_the_kept_ability_casts(self):
        lines = self.stage_ini.read_bytes().split(b"\r\n")
        active = [l for l in lines
                  if l.lstrip().startswith(b"$SIBADA") and not l.lstrip().startswith(b";")]
        self.assertEqual(active[0], REQUEST_PAYLOAD)
        self.assertEqual([form_token(l) for l in active[1:]],
                         [t for t in KEPT_FORM_TOKENS])
        self.assertEqual(len(active), 1 + len(KEPT_FORM_TOKENS))
        self.assertNotIn("0X814", " ".join(l.decode() for l in active))
        self.assertNotIn("0X2FFD2", " ".join(l.decode() for l in active))
        self.assertNotIn("0X7A82B", " ".join(l.decode() for l in active))

    def test_kept_payloads_are_byte_identical_to_the_original_lines(self):
        if not self.original.is_file():
            self.skipTest(f"original ini not present: {self.original}")
        original = self.original.read_bytes().split(b"\r\n")
        staged = self.stage_ini.read_bytes().split(b"\r\n")
        for token in KEPT_FORM_TOKENS:
            source_line = next(l for l in original if form_token(l) == token)
            staged_line = next(l for l in staged if form_token(l) == token)
            self.assertEqual(staged_line, source_line)

    def test_not_castable_payload_is_the_only_one_not_applied(self):
        lines = self.stage_ini.read_bytes().split(b"\r\n")
        for token in DROPPED_FORM_TOKENS:
            self.assertEqual([l for l in lines if form_token(l) == token], [])
            self.assertNotIn(token, " ".join(l.decode("ascii", "replace") for l in lines
                                             if not l.lstrip().startswith(b";")))

    def test_commented_lines_survive(self):
        lines = self.stage_ini.read_bytes().split(b"\r\n")
        commented = [l for l in lines if l.lstrip().startswith(b";$SIBADA")]
        self.assertEqual(len(commented), self.pi["commentedLinesKept"])
        self.assertEqual(len(commented), 2)

    def test_byte_identical_outside_the_replaced_block(self):
        if not self.original.is_file():
            self.skipTest(f"original ini not present: {self.original}")
        original = self.original.read_bytes()
        patched = self.stage_ini.read_bytes()
        splitter = b"\r\n" if b"\r\n" in original else b"\n"
        self.assertIn(splitter, patched)
        expected, active, keep = rebuild_sibada_block(original.split(splitter), REQUEST_PAYLOAD,
                                                      KEPT_FORM_TOKENS)
        self.assertEqual(len(active), 7)
        self.assertEqual([form_token(original.split(splitter)[i]) for i in keep],
                         list(KEPT_FORM_TOKENS))
        self.assertEqual(patched, splitter.join(expected))
        # every line that is not part of the replaced block is byte-identical
        original_rest = [l for i, l in enumerate(original.split(splitter)) if i not in active]
        patched_rest = [l for l in patched.split(splitter)
                        if l != REQUEST_PAYLOAD and form_token(l) not in KEPT_FORM_TOKENS
                        and form_token(l) not in DROPPED_FORM_TOKENS]
        self.assertEqual(original_rest, patched_rest)

    def test_report_matches_the_staged_ini(self):
        self.assertEqual(self.pi["requestPayload"], REQUEST_PAYLOAD.decode())
        self.assertEqual([form_token(l.encode()) for l in self.pi["keptPayloads"]],
                         list(KEPT_FORM_TOKENS))
        self.assertEqual([form_token(l.encode()) for l in self.pi["droppedPayloads"]],
                         list(DROPPED_FORM_TOKENS))
        self.assertEqual(len(self.pi["replacedPayloads"]), 4)
        self.assertEqual(self.pi["activePayloadLines"],
                         len(self.pi["keptPayloads"]) + len(self.pi["droppedPayloads"])
                         + len(self.pi["replacedPayloads"]))


# --------------------------------------------------------------------------- integration.json

class TestIntegrationFacts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.integration = load_json(INTEGRATION)
        cls.record = load_json(RECORD_REPORT)
        cls.referenced = {e["requested"]: e for e in cls.record["referencedForms"]}
        cls.candidates = {c["localID"]: c for c in cls.record["attackCandidates"]}

    def test_payload_entries_match_the_real_record_types_and_cast_parameters(self):
        for entry in self.integration["originalPayloads"]:
            form = entry["requestedForm"]
            if entry["resolvedRecordType"] is None:
                self.assertFalse(entry["active"], form or entry["piFormToken"])
                continue
            resolved = self.referenced.get(form)
            self.assertIsNotNone(resolved, f"{form} not in the analyzer report")
            self.assertTrue(resolved["found"], form)
            self.assertEqual(entry["resolvedRecordType"], resolved["recordType"], form)
            self.assertEqual(entry["editorID"], resolved["editorID"], form)
            if resolved["recordType"] == "SPEL":
                spell = resolved["spell"]
                self.assertEqual(entry["realType"], spell["type"], form)
                self.assertEqual(entry["castType"], spell["castType"], form)
                self.assertEqual(entry["targetType"], spell["targetType"], form)
                self.assertEqual(entry["baseCost"], spell["baseCost"], form)
                self.assertEqual(entry["effectCount"], spell["effectCount"], form)
            else:
                effect = resolved["magicEffect"]
                self.assertEqual(entry["realType"], "MagicEffect", form)
                self.assertEqual(entry["castType"], effect["castType"], form)
                self.assertEqual(entry["targetType"], effect["targetType"], form)

    def test_ability_payloads_are_not_called_ineffective(self):
        by_form = {e["requestedForm"]: e for e in self.integration["originalPayloads"]
                   if e["requestedForm"]}
        for form in ("God of War.esp:0x817", "God of War.esp:0x844"):
            entry = by_form[form]
            self.assertEqual(entry["realType"], "Ability")
            self.assertEqual(entry["patchAction"], "keptVerbatim", form)
            note = entry["note"].lower()
            # it must be recorded as castable, and never described as having no effect
            self.assertIn("castspellimmediate", note, form)
            for claim in ("has no effect", "does nothing", "is ineffective",
                          "proven ineffective", "no gameplay effect"):
                self.assertNotIn(claim, note, f"{form}: {claim}")
            self.assertIn("kept", note, form)
        self.assertEqual(by_form["God of War.esp:0x817"]["baseCost"], 1084)
        self.assertEqual(by_form["God of War.esp:0x817"]["castType"], "FireAndForget")
        self.assertEqual(by_form["God of War.esp:0x817"]["realType"], "Ability")

    def test_only_the_wrong_record_type_payload_is_not_applied(self):
        not_applied = [e["requestedForm"] for e in self.integration["originalPayloads"]
                       if e["patchAction"] == "notAppliedNotCastable"]
        self.assertEqual(not_applied, ["Dragonborn.esm:0x1DF97"])
        entry = next(e for e in self.integration["originalPayloads"]
                     if e["requestedForm"] == "Dragonborn.esm:0x1DF97")
        self.assertEqual(entry["resolvedRecordType"], "MGEF")
        self.assertIn("must not be upgraded", entry["note"])

    def test_payload_summary_matches_the_entries_and_the_staged_ini(self):
        summary = self.integration["originalPayloadSummary"]
        entries = self.integration["originalPayloads"]
        self.assertEqual(summary["activeLines"], sum(1 for e in entries if e["active"]))
        self.assertEqual(summary["commentedLines"], sum(1 for e in entries if not e["active"]))
        self.assertEqual(summary["keptVerbatim"],
                         [e["requestedForm"] for e in entries
                          if e["patchAction"] == "keptVerbatim"])
        self.assertEqual(summary["replacedByRequestCast"],
                         [e["requestedForm"] for e in entries
                          if e["patchAction"] == "replacedByRequestCast"])
        self.assertEqual(summary["notCastable"],
                         [e["requestedForm"] for e in entries
                          if e["patchAction"] == "notAppliedNotCastable"])
        self.assertIn("castable", summary["policy"].lower())
        pi = load_json(OAR_REPORT)["payloadInterpreter"]
        self.assertEqual(pi["requestPayload"], self.integration["activationRequest"]["writtenPayload"])

    # ---------------------------------------------------------------- whitelist curation

    def whitelist_forms(self):
        whitelist = self.integration["hitChannels"]["scoringWhitelist"]
        forms, channels = {}, {}
        for channel, group in whitelist.items():
            for form in group["forms"]:
                forms[form["localID"]] = form
                channels[form["localID"]] = channel
        return forms, channels

    def excluded_forms(self):
        return {e["localID"]: e for e in self.integration["hitChannels"]["excluded"]}

    def test_candidates_are_kept_as_non_authoritative_reference(self):
        candidates = self.integration["hitChannels"]["candidates"]
        self.assertEqual(candidates["count"], len(self.candidates))
        self.assertEqual(
            sorted(f["localID"] for f in candidates["forms"]),
            sorted(self.candidates))
        for form in candidates["forms"]:
            self.assertEqual(form["autoHint"], self.candidates[form["localID"]]["channel"]
                             or f"excluded({self.candidates[form['localID']]['exclusionReason']})")
        self.assertIn("supersede", candidates["note"])

    def test_whitelist_and_excluded_partition_the_candidates(self):
        whitelist, _ = self.whitelist_forms()
        excluded = self.excluded_forms()
        self.assertEqual(set(whitelist) | set(excluded), set(self.candidates))
        self.assertEqual(set(whitelist) & set(excluded), set())

    def test_whitelist_forms_match_the_analyzer_record_fields(self):
        whitelist, _ = self.whitelist_forms()
        for local, form in whitelist.items():
            candidate = self.candidates[local]
            self.assertEqual(candidate["editorID"], form["editorID"], local)
            self.assertEqual(candidate["spellType"], form["type"], local)
            self.assertEqual(candidate["castType"], form["castType"], local)
            self.assertEqual(candidate["targetType"], form["targetType"], local)
            self.assertEqual(candidate["baseCost"], form["baseCost"], local)

    def test_buffs_dust_and_barehand_swap_are_never_in_the_whitelist(self):
        whitelist, _ = self.whitelist_forms()
        for local in BUFFS_AND_SWAP | DUST | PERSISTENT_AREA:
            self.assertNotIn(local, whitelist, local)
        excluded = self.excluded_forms()
        for local in BUFFS_AND_SWAP | DUST:
            self.assertIn(local, excluded, local)
        reasons = {local: excluded[local]["reason"] for local in BUFFS_AND_SWAP | DUST}
        self.assertEqual(reasons["0x817"], "barehandSwapSelfBuff")
        self.assertEqual(reasons["0x82C"], "selfBuff")
        self.assertEqual(reasons["0x844"], "selfBuff")
        self.assertEqual(reasons["0x847"], "selfBuff")
        self.assertEqual(reasons["0x848"], "selfBuff")
        self.assertEqual({reasons[l] for l in DUST}, {"dust"})

    def test_no_ability_or_self_buff_ever_reaches_the_whitelist(self):
        whitelist, _ = self.whitelist_forms()
        for local, form in whitelist.items():
            self.assertNotEqual(form["type"], "Ability", local)
            self.assertNotIn(
                self.candidates[local]["exclusionReason"], ("dust", "cloak"), local)
            if form["targetType"] == "Self":
                # a self-targeted spell may only score when it is a launched one-shot
                self.assertTrue(self.candidates[local]["projectiles"], local)

    def test_persistent_area_is_not_accepted_just_for_carrying_a_projectile(self):
        _, channels = self.whitelist_forms()
        self.assertNotIn("0x843", channels)
        self.assertEqual(self.excluded_forms()["0x843"]["reason"], "persistentSelfCentredArea")
        self.assertTrue(self.candidates["0x843"]["projectiles"])
        # and the reverse control: launched one-shot projectiles are in the projectile channel
        for local in ("0x82B", "0x840", "0x842", "0x86E"):
            self.assertEqual(channels[local], "SubmitProjectileHit", local)
        for local in ("0x81F", "0x841", "0x86D"):
            self.assertEqual(channels[local], "SubmitMagicHit", local)

    def test_weapon_channel_holds_no_spell_of_this_plugin(self):
        whitelist = self.integration["hitChannels"]["scoringWhitelist"]
        self.assertEqual(whitelist["SubmitWeaponHit"]["forms"], [])
        self.assertIn("no form of this plugin", whitelist["SubmitWeaponHit"]["rule"])
        for group in ("SubmitMagicHit", "SubmitProjectileHit"):
            self.assertTrue(whitelist[group]["rule"])

    def test_oar_gate_forms_match_the_analyzer(self):
        for gate in self.integration["oarAnimationGates"]["referencedForms"]:
            resolved = self.referenced.get(gate["requested"])
            self.assertIsNotNone(resolved, gate["requested"])
            self.assertEqual(resolved["recordType"], gate["recordType"], gate["requested"])
            self.assertEqual(resolved["editorID"], gate["editorID"], gate["requested"])

    def test_ready_groups_in_the_facts_match_the_generated_submods(self):
        groups = self.integration["oarAnimationGates"]["readyGroups"]
        generated = {r["submod"]: r for r in load_json(OAR_REPORT)["oarNewSubmods"]}
        self.assertEqual(sorted(g["submod"] for g in groups), sorted(generated))
        for group in groups:
            staged = generated[group["submod"]]
            self.assertEqual(group["sourceSubmod"], staged["sourceSubmod"])
            self.assertEqual(group["priority"], staged["priority"])
            self.assertEqual(group["overrideAnimationsFolder"],
                             staged["overrideAnimationsFolder"])
            self.assertEqual(
                group["addedCondition"],
                "HasMagicEffect KratosRagePatch.esp "
                f"0x{staged['appendedCondition']['Magic effect']['formID']}")
            self.assertIn("original condition", group["conditionsKept"])
            if group["sourceSubmod"] == "SpartanRageTrigger":
                self.assertIn("Keytrace", group["conditionsKept"])
        policy = self.integration["oarAnimationGates"]["policy"]
        self.assertIn("never removes an original condition", policy)
        self.assertIn("overrideAnimationsFolder", policy)


# --------------------------------------------------------------------------- stage layout

class TestStageLayout(unittest.TestCase):
    def test_expected_top_level_entries(self):
        expected = {"KratosRagePatch.esp", "Scripts", "Seq", "Interface", "SKSE", "meshes"}
        actual = {p.name for p in STAGE.iterdir()}
        self.assertEqual(actual, expected)

    def test_widget_swf_is_present(self):
        swf = STAGE / "Interface" / "exported" / "widgets" / "KratosRage" / "meter.swf"
        self.assertTrue(swf.is_file())
        self.assertEqual(swf.read_bytes()[:3], b"FWS")

    def test_no_stray_json_outside_the_oar_tree(self):
        oar_tree = STAGE / OAR_REL
        config = STAGE / "SKSE" / "Plugins" / "StorageUtilData" / "KratosRage" / "config.json"
        junk = [str(p.relative_to(STAGE)) for p in STAGE.rglob("*.json")
                if not p.is_relative_to(oar_tree) and p != config]
        self.assertEqual(junk, [], f"unexpected staging files: {junk}")
        if config.is_file():
            self.assertEqual(config.read_bytes(), (REPO / "data" / "config.default.json").read_bytes())

    def test_stage_does_not_touch_the_original_mod(self):
        report = load_json(OAR_REPORT)
        mod = Path(report["originalMod"])
        if not mod.is_dir():
            self.skipTest(f"original mod not present: {mod}")
        oar = (mod / "meshes" / "actors" / "character" / "animations"
               / "OpenAnimationReplacer" / "KratosMoveset")
        for submod in EXPECTED_DISABLES:
            original = load_json(oar / submod / "config.json")
            self.assertNotIn("replacementAnimDatas", original,
                             f"{submod}/config.json was modified - the original mod is read-only")
        self.assertFalse((oar / f"{READY_SUFFIX}").exists())


if __name__ == "__main__":
    if not PLUGIN.is_file():
        print(f"error: {PLUGIN} not found - run tools/PluginBuilder build first", file=sys.stderr)
        raise SystemExit(2)
    if not OAR_REPORT.is_file():
        print(f"error: {OAR_REPORT} not found - run tools/generate_oar.py first", file=sys.stderr)
        raise SystemExit(2)
    unittest.main(verbosity=2)
