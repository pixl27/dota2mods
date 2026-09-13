r"""
refresh_profile.py - re-locates the native appearance profile in a new client.dll
and regenerates src/native_appearance_profile.h (plus data/native_profile.json).

usage: python refresh_profile.py [--client PATH] [--check] [--seed NAME=RVA ...]

How entries are found, in order:
1. masked byte signature (unique in .text) at the last known address, then anywhere;
2. a call site inside an already located function ("call" sites);
3. string / call-graph anchors, used when a signature stopped matching, which
   also record a fresh signature.
Globals, struct offsets and vtable slots are read from "sites": instructions
inside located functions whose displacement or immediate carries the value.
Schema offsets (m_steamID, m_hAssignedHero) come from the schema registration
code next to the field name string.

The DLL performs the same signature/site resolution at load time, so a Dota
update that only shifts code keeps working without a rebuild. Run this script
(then build.bat) when the DLL reports an unresolved entry.

Requires: pip install pefile capstone numpy
"""
import argparse
import bisect
import hashlib
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

# build.bat copies this script into build\ next to the binaries. A copy run from
# there would otherwise default to writing build\data\native_profile.json and
# build\src\native_appearance_profile.h — the second directory does not exist, and
# both are a mirror the next rebuild overwrites from the project root anyway. So
# resolve the defaults against the project the copy came from, using the same
# build\-detection as the launcher (see src/app/environment.h).
ROOT = HERE.parent if HERE.name == "build" and (HERE.parent / "build.bat").exists() else HERE

try:
    import numpy as np
    import pefile
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP
except ImportError as error:
    print(f"[!] missing package ({error.name}): pip install pefile capstone numpy")
    sys.exit(1)

# Resolution order matters: a function located through a call site needs its parent first.
FUNCTIONS = ["BuildWearableList", "ModelOverride", "CombineModels", "Spawn", "Think", "BuildSpawnWearableList", "WearablePlayer",
             "InventoryForPlayer", "EquippedView", "DefaultView", "FindItemView", "KeyValuesModel", "SetModel", "CreateWearables",
             "SetBaseModel", "ModelHandle", "ModelReady", "ViewModel", "ResourceNameCtor", "ResourceNameIsType", "EntityModelLookup",
             "EntityModelModifiers", "KeyValuesCtor", "PrepareWearables", "KeyValuesDtor", "ModifierManager", "PreviewBuilder",
             "ModifierCtor", "PopulateModifiers", "Allocate", "LocalControllerGetter", "ModelNameLookup", "DestroyWearables"]
GLOBALS = ["LocalController", "EntityChunks", "InventoryManager", "ModelInfo", "ResourceSystem", "ResourceSystemInterface", "JustInTimeManifest"]
OFFSETS = ["SteamId", "AssignedHero", "PlayerId", "HeroId", "DirtyFlags", "BaseModel", "ModelIndex", "Modifiers", "CreationList",
           "CreationInitialized", "WearableCount", "ModifierPath", "ModelRefCount"]
SLOTS = ["ResourceStateSlot", "ResourceRegisterSlot", "ResourceFindSlot", "FindOrLoadSlot", "ReleaseSlot", "ModelNameSlot"]
OFFSETS.append("ModifierStorage")
GLOBALS.append("MemAllocImport")


class Image:
    def __init__(self, path):
        self.path = Path(path)
        self.pe = pefile.PE(str(path), fast_load=True)
        self.data = self.path.read_bytes()
        self.array = np.frombuffer(self.data, dtype=np.uint8)
        self.sections = [(s.Name.rstrip(b"\0").decode(), s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData) for s in self.pe.sections]
        self.text = next(s for s in self.sections if s[0] == ".text")
        self.md = Cs(CS_ARCH_X86, CS_MODE_64)
        self.md.detail = True
        # Chained unwind entries continue a function; only unchained ones start
        # one. There are 200k of them, so .pdata is read as an array rather than
        # one Python object per record: the difference is seconds against
        # milliseconds, on a path every maintenance action waits for.
        self.starts = []
        pdata = next((s for s in self.sections if s[0] == ".pdata"), None)
        if pdata:
            _, va, vs, po, ps = pdata
            records = min(vs, ps) // 12
            fields = np.frombuffer(self.data, dtype=np.uint32, count=records * 3, offset=po).astype(np.int64)
            begins, unwinds = fields[0::3], fields[2::3]
            offsets = np.full(records, -1, dtype=np.int64)
            for _, sva, svs, spo, sps in self.sections:
                inside = (unwinds >= sva) & (unwinds < sva + max(svs, sps))
                offsets[inside] = spo + (unwinds[inside] - sva)
            known = (offsets >= 0) & (offsets < len(self.data))
            flags = np.zeros(records, dtype=np.uint8)
            flags[known] = self.array[offsets[known]]
            self.starts = sorted(set(begins[known & (((flags >> 3) & 4) == 0)].tolist()))
        self._calls = None
        self.sha256 = hashlib.sha256(self.data).hexdigest()

    def offset(self, rva):
        for name, va, vs, po, ps in self.sections:
            if va <= rva < va + max(vs, ps):
                return po + rva - va
        return None

    def rva(self, offset):
        for name, va, vs, po, ps in self.sections:
            if po <= offset < po + ps:
                return va + offset - po
        return None

    def read(self, rva, size):
        off = self.offset(rva)
        return self.data[off:off + size] if off is not None else b""

    def cstring(self, rva, limit=200):
        raw = self.read(rva, limit)
        end = raw.find(b"\0")
        return raw[:end].decode("latin1") if end >= 0 else None

    def function_start(self, rva):
        i = bisect.bisect_right(self.starts, rva) - 1
        return self.starts[i] if i >= 0 else None

    def function_end(self, rva):
        i = bisect.bisect_right(self.starts, rva)
        return self.starts[i] if i < len(self.starts) else rva + 0x2000

    def disasm(self, rva, size=None):
        end = rva + size if size else self.function_end(rva)
        return list(self.md.disasm(self.read(rva, end - rva), rva))

    def find_string(self, text):
        needle = b"\0" + text.encode() + b"\0"
        hits, start = [], 0
        while True:
            i = self.data.find(needle, start)
            if i < 0:
                break
            hits.append(self.rva(i + 1))
            start = i + 1
        return [h for h in hits if h is not None]

    def rip_xrefs(self, target):
        """Code sites (lea/mov/cmp with a rip-relative operand) referencing target."""
        name, va, vs, po, ps = self.text
        seg = self.array[po:po + ps]
        idx = np.flatnonzero((seg == 0x8D) | (seg == 0x8B) | (seg == 0x89) | (seg == 0x3B) | (seg == 0x39) | (seg == 0x83))
        idx = idx[idx + 6 <= len(seg)]
        idx = idx[(seg[idx + 1] & 0xC7) == 0x05]
        rel = seg[idx[:, None] + np.arange(2, 6)].astype(np.uint32)
        rel = (rel[:, 0] | (rel[:, 1] << 8) | (rel[:, 2] << 16) | (rel[:, 3] << 24)).astype(np.int32)
        tgt = (va + idx + 6).astype(np.int64) + rel + (seg[idx] == 0x83)  # cmp [rip+d], imm8 is one byte longer
        return sorted(int(va + i) for i in idx[tgt == target])

    def call_sites(self):
        if self._calls is None:
            name, va, vs, po, ps = self.text
            seg = self.array[po:po + ps]
            idx = np.flatnonzero(seg == 0xE8)
            idx = idx[idx + 5 <= len(seg)]
            rel = seg[idx[:, None] + np.arange(1, 5)].astype(np.uint32)
            rel = (rel[:, 0] | (rel[:, 1] << 8) | (rel[:, 2] << 16) | (rel[:, 3] << 24)).astype(np.int32)
            self._calls = (va + idx, (va + idx + 5).astype(np.int64) + rel)
        return self._calls

    def callers(self, target):
        sites, targets = self.call_sites()
        return sorted(int(s) for s in sites[targets == target])

    def functions_referencing(self, string_rva):
        return sorted({self.function_start(x) for x in self.rip_xrefs(string_rva) if self.function_start(x)})

    def scan(self, pattern, mask):
        """All .text rvas where pattern matches under mask (0xff = compare)."""
        name, va, vs, po, ps = self.text
        seg = self.array[po:po + ps]
        first = next(i for i, m in enumerate(mask) if m == 0xFF)
        candidates = np.flatnonzero(seg == pattern[first]) - first
        candidates = candidates[(candidates >= 0) & (candidates + len(pattern) <= len(seg))]
        pat = np.frombuffer(bytes(pattern), dtype=np.uint8)
        msk = np.frombuffer(bytes(mask), dtype=np.uint8)
        hits = []
        for start in range(0, len(candidates), 65536):
            chunk = candidates[start:start + 65536]
            window = seg[chunk[:, None] + np.arange(len(pattern))]
            ok = np.all((window & msk) == (pat & msk), axis=1)
            hits.extend(int(va + c) for c in chunk[ok])
        return hits


def masked_bytes(image, rva, length):
    """Instruction bytes with every 32-bit immediate/displacement and branch target masked."""
    pattern, mask = bytearray(), bytearray()
    for ins in image.md.disasm(image.read(rva, length + 16), rva):
        if len(pattern) >= length:
            break
        raw, keep = bytearray(ins.bytes), bytearray(b"\xff" * ins.size)
        if ins.disp_size == 4 and ins.disp_offset:
            keep[ins.disp_offset:ins.disp_offset + 4] = b"\0" * 4
        if ins.imm_size == 4 and ins.imm_offset:
            keep[ins.imm_offset:ins.imm_offset + 4] = b"\0" * 4
        if (ins.mnemonic in ("call", "jmp") or ins.mnemonic.startswith("j")) and ins.imm_size:
            keep[ins.imm_offset:ins.imm_offset + ins.imm_size] = b"\0" * ins.imm_size
        pattern += raw
        mask += keep
    return bytes(pattern), bytes(mask)


def unique_signature(image, rva):
    end = image.function_end(rva)
    for length in (0x18, 0x20, 0x30, 0x40, 0x60):
        if rva + length > end + 8:
            break
        pattern, mask = masked_bytes(image, rva, length)
        if image.scan(pattern, mask) == [rva]:
            return pattern, mask
    return None, None


# --------------------------------------------------------------------------- helpers
def mem_disp(ins, base_reg=None):
    for op in ins.operands:
        if op.type == X86_OP_MEM and (base_reg is None or ins.reg_name(op.mem.base) == base_reg):
            return op.mem.disp
    return None


def rip_target(ins):
    for op in ins.operands:
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
            return ins.address + ins.size + op.mem.disp
    return None


def direct_call_target(ins):
    if ins.mnemonic == "call" and ins.operands and ins.operands[0].type == X86_OP_IMM:
        return ins.operands[0].imm
    return None


def text_of(ins):
    return f"{ins.mnemonic} {ins.op_str}"


def decode_site(site, window, window_rva):
    """The value a site window carries, exactly as the DLL's resolver reads it."""
    raw = window[site["operand"]:site["operand"] + site["width"]]
    value = int.from_bytes(raw, "little", signed=True)
    if site["kind"] in ("Rip", "Call"):
        return window_rva + site["end"] + value
    if site["kind"] == "Slot":
        return value // 8
    return value  # Disp, Imm and Schema all carry the value directly


class Locator:
    def __init__(self, image, previous):
        self.image = image
        self.previous = previous if previous is not None else {}
        self.found = {}
        self.callsite = {}   # name -> (parent, call rva)
        self.report = []
        self.code = {}
        self.values = {}

    def body(self, name):
        if name not in self.code:
            self.code[name] = self.image.disasm(self.found[name])
        return self.code[name]

    def by_signature(self, name):
        entry = self.previous.get("functions", {}).get(name)
        if not entry or not entry.get("signature"):
            return None
        pattern, mask = bytes.fromhex(entry["signature"]), bytes.fromhex(entry["mask"])
        hint = int(entry["rva"], 16)
        masked = lambda raw: bytes(a & m for a, m in zip(raw, mask))
        if masked(self.image.read(hint, len(pattern))) == masked(pattern):
            return hint
        hits = self.image.scan(pattern, mask)
        if len(hits) == 1:
            self.report.append(f"{name}: moved {hint:#x} -> {hits[0]:#x} (signature)")
            return hits[0]
        return None

    def string_function(self, *strings):
        sets = []
        for s in strings:
            funcs = set()
            for r in self.image.find_string(s):
                funcs |= set(self.image.functions_referencing(r))
            sets.append(funcs)
        common = set.intersection(*sets) if sets else set()
        return min(common, key=lambda f: self.image.function_end(f) - f) if common else None

    def find_call(self, parent, predicate, after=0):
        """(target, index) of the first direct call in parent for which predicate(index, code) holds."""
        code = self.body(parent)
        for i in range(after, len(code)):
            target = direct_call_target(code[i])
            if target is not None and predicate(i, code):
                return target, i
        return None, None

    def via_call(self, name, parent, predicate, after=0):
        target, index = self.find_call(parent, predicate, after)
        if target is not None:
            self.callsite[name] = (parent, self.body(parent)[index].address)
        return target

    def locate(self, name, anchor):
        rva = self.by_signature(name)
        if rva is None and anchor is not None:
            try:
                rva = anchor()
            except Exception as error:  # noqa: BLE001 - reported below
                self.report.append(f"{name}: anchor failed ({error!r})")
                rva = None
            if rva is not None:
                self.report.append(f"{name}: located by anchor at {rva:#x}")
        elif rva is not None and anchor is not None:
            try:
                anchor()  # still record the call site for the header
            except Exception:  # noqa: BLE001
                pass
        if rva is None:
            self.report.append(f"{name}: NOT FOUND")
            return None
        self.found[name] = rva
        return rva

    def run(self):
        im, L = self.image, self.locate
        L("BuildWearableList", lambda: self.string_function("altItemIndex"))
        L("ModelOverride", lambda: self.string_function("%s_variant_%d"))
        L("CombineModels", lambda: self.string_function("npc_dota_hero_tiny", "JustInTimeManifest"))
        L("Spawn", lambda: self.string_function("hero_footstep", "IsRoshan"))

        def think():
            callees = set()
            for r in im.find_string("ClientReplacementModel_ReplaceAnimations"):
                callees |= set(im.functions_referencing(r))
            for r in im.find_string("particles/speechbubbles/speech_voice.vpcf"):
                for f in im.functions_referencing(r):
                    if any(direct_call_target(x) in callees for x in im.disasm(f)):
                        return f
            return None
        L("Think", think)
        L("BuildSpawnWearableList", lambda: self.via_call("BuildSpawnWearableList", "BuildWearableList", lambda i, c: True))
        L("WearablePlayer", lambda: self.via_call("WearablePlayer", "BuildWearableList", lambda i, c: text_of(c[i + 1]).startswith("mov esi, dword ptr [rsp")))
        L("InventoryForPlayer", lambda: self.via_call("InventoryForPlayer", "BuildWearableList", lambda i, c: any(text_of(x) == "xor r8d, r8d" for x in c[i - 3:i]) and any(text_of(x) == "mov edx, esi" for x in c[i - 3:i])))
        L("EquippedView", lambda: self.via_call("EquippedView", "BuildWearableList", lambda i, c: any(text_of(x) == "xor r9d, r9d" for x in c[i - 3:i]) and any(x.mnemonic == "mov" and mem_disp(x) is not None and "edx" in x.op_str for x in c[i - 3:i])))
        L("DefaultView", lambda: self.via_call("DefaultView", "BuildWearableList", lambda i, c: text_of(c[i - 1]) == "mov rcx, rax" and any(text_of(x) == "mov r8d, r14d" for x in c[i - 4:i])))
        L("FindItemView", lambda: self.via_call("FindItemView", "EquippedView", lambda i, c: any(text_of(x) == "xor r8d, r8d" for x in c[i - 3:i])))

        def spawn_index():
            code = self.body("Spawn")
            return next(i for i, ins in enumerate(code) if direct_call_target(ins) == self.found["ModelOverride"])

        def before_override(name):
            code, idx = self.body("Spawn"), spawn_index()
            calls = [i for i in range(idx) if direct_call_target(code[i]) is not None]
            self.callsite[name] = ("Spawn", code[calls[-1]].address)
            return direct_call_target(code[calls[-1]])

        def after_override(name, nth):
            code, idx = self.body("Spawn"), spawn_index()
            calls = [i for i in range(idx + 1, len(code)) if direct_call_target(code[i]) is not None]
            self.callsite[name] = ("Spawn", code[calls[nth]].address)
            return direct_call_target(code[calls[nth]])
        L("KeyValuesModel", lambda: before_override("KeyValuesModel"))
        L("SetModel", lambda: after_override("SetModel", 0))
        L("CreateWearables", lambda: after_override("CreateWearables", 1))
        L("SetBaseModel", lambda: self.via_call("SetBaseModel", "CombineModels", lambda i, c: any(x.mnemonic == "cmp" and "rdx" in x.op_str and mem_disp(x, "r14") for x in c[i - 4:i])))
        L("ModelHandle", lambda: self.via_call("ModelHandle", "CombineModels", lambda i, c: any(x.mnemonic == "test" and "0x10" in x.op_str and mem_disp(x, "r14") for x in c[i - 6:i])))
        L("ModelReady", lambda: self.via_call("ModelReady", "CombineModels", lambda i, c: text_of(c[i - 1]).startswith("mov rcx, qword ptr [r14 +")))
        L("ViewModel", lambda: self.via_call("ViewModel", "CombineModels", lambda i, c: any(x.mnemonic == "mov" and "edx" in x.op_str and mem_disp(x, "r14") for x in c[i - 4:i]) and text_of(c[i - 1]) == "mov rcx, r12"))
        L("ResourceNameCtor", lambda: self.via_call("ResourceNameCtor", "CombineModels", lambda i, c: any("0xc00000c8" in text_of(x) for x in c[i - 6:i])))
        L("ResourceNameIsType", lambda: self.via_call("ResourceNameIsType", "CombineModels", lambda i, c: any(text_of(x) == "mov edx, 0x6c646d76" for x in c[i - 3:i])))
        L("EntityModelLookup", lambda: self.via_call("EntityModelLookup", "ModelOverride", lambda i, c: True))
        L("EntityModelModifiers", lambda: self.via_call("EntityModelModifiers", "EntityModelLookup", lambda i, c: True))
        L("KeyValuesCtor", lambda: self.via_call("KeyValuesCtor", "CreateWearables", lambda i, c: any(text_of(x) == "mov r8b, 2" for x in c[i - 5:i])))
        # BuildWearableList is virtual; PrepareWearables is the first direct call after the key-values constructor.
        L("PrepareWearables", lambda: self.via_call("PrepareWearables", "CreateWearables", lambda i, c: True,
                                                  after=self.find_call("CreateWearables", lambda i, c: direct_call_target(c[i]) == self.found["KeyValuesCtor"])[1] + 1))

        def kv_dtor():
            code = self.body("CreateWearables")
            last = [i for i, x in enumerate(code) if direct_call_target(x) is not None][-1]
            self.callsite["KeyValuesDtor"] = ("CreateWearables", code[last].address)
            return direct_call_target(code[last])
        L("KeyValuesDtor", kv_dtor)

        def is_global_getter(target):
            body = im.disasm(target, 0x20)
            return len(body) >= 2 and body[0].mnemonic == "lea" and rip_target(body[0]) and any(x.mnemonic == "ret" for x in body[:4])
        L("ModifierManager", lambda: self.via_call("ModifierManager", "CreateWearables", lambda i, c: is_global_getter(direct_call_target(c[i]))))

        def preview_builder():
            for site in im.callers(self.found["PrepareWearables"]):
                f = im.function_start(site)
                if f != self.found["CreateWearables"]:
                    return f
            return None
        L("PreviewBuilder", preview_builder)

        def allocation_index():
            code = self.body("PreviewBuilder")
            idx = next(i for i, x in enumerate(code) if text_of(x) == "mov ecx, 0x30")
            return self.find_call("PreviewBuilder", lambda i, c: True, after=idx)[1]
        # The modifier list is heap allocated, then constructed in place.
        L("Allocate", lambda: self.via_call("Allocate", "PreviewBuilder", lambda i, c: True, after=allocation_index()))
        L("ModifierCtor", lambda: self.via_call("ModifierCtor", "PreviewBuilder", lambda i, c: True, after=allocation_index() + 1))

        def populate():
            code = self.body("PreviewBuilder")
            idx = next(i for i, x in enumerate(code) if direct_call_target(x) == self.found["ModifierCtor"])
            mgr = self.find_call("PreviewBuilder", lambda i, c: direct_call_target(c[i]) == self.found["ModifierManager"], after=idx + 1)[1]
            return self.via_call("PopulateModifiers", "PreviewBuilder", lambda i, c: True, after=mgr + 1)
        L("PopulateModifiers", populate)
        L("LocalControllerGetter", lambda: self.via_call("LocalControllerGetter", "Spawn", lambda i, c: text_of(c[i - 1]) == "xor ecx, ecx"))
        # Only needed for the model-name vtable slot, which is otherwise a guess.
        L("ModelNameLookup", lambda: self.string_function("particles/world_destruction_fx/tree_destroy.vpcf", "tree_destruction_generic"))
        L("DestroyWearables", None)
        return self.found

    # ----------------------------------------------------------------- sites
    def window(self, function, index, operand, width, minimum=14):
        """Bytes around code[index] with every variable field masked.

        A site is re-found by scanning the containing function for this window,
        so it carries enough surrounding context to stay unique there even when
        the operand itself (the value being read) changes.
        """
        code = self.body(function)
        first, size = index, len(code[index].bytes)
        while first > 0 and size < minimum:
            first -= 1
            size += len(code[first].bytes)
        pattern, mask, target_at = bytearray(), bytearray(), 0
        for i in range(first, index + 1):
            ins = code[i]
            if i == index:
                target_at = len(pattern)
            raw, keep = bytearray(ins.bytes), bytearray(b"\xff" * ins.size)
            for start, length in ((ins.disp_offset, ins.disp_size), (ins.imm_offset, ins.imm_size)):
                if start and length == 4:
                    keep[start:start + 4] = b"\0" * 4
            if (ins.mnemonic in ("call", "jmp") or ins.mnemonic.startswith("j")) and ins.imm_size:
                keep[ins.imm_offset:ins.imm_offset + ins.imm_size] = b"\0" * ins.imm_size
            pattern += raw
            mask += keep
        mask[target_at + operand:target_at + operand + width] = b"\0" * width
        return {"function": function, "offset": code[first].address - self.found[function], "bytes": bytes(pattern).hex(),
                "mask": bytes(mask).hex(), "size": len(pattern), "operand": target_at + operand,
                "end": target_at + len(code[index].bytes), "width": width, "index": index}

    def unique_in_function(self, name, site):
        """Widen the window until it appears exactly once inside its function."""
        function, index = site["function"], site["index"]
        code = self.body(function)
        start = self.found[function]
        body = self.image.read(start, self.image.function_end(start) - start)
        operand_in_instruction = site["operand"] - (site["end"] - len(code[index].bytes))
        for minimum in (14, 26, 44, 72):
            pattern, mask = bytes.fromhex(site["bytes"]), bytes.fromhex(site["mask"])
            hits = 0
            for at in range(len(body) - len(pattern) + 1):
                if all((body[at + k] & mask[k]) == (pattern[k] & mask[k]) for k in range(len(pattern))):
                    hits += 1
                    if hits > 1:
                        break
            if hits == 1 or self.same_value_everywhere(site, body, pattern, mask):
                return site
            widened = self.window(function, index, operand_in_instruction, site["width"], minimum=minimum + 12)
            if widened["size"] == site["size"]:
                break
            site = dict(site, **widened)
        self.report.append(f"{name}: window is not unique inside {function}")
        return site

    def same_value_everywhere(self, site, body, pattern, mask):
        start = self.found[site["function"]]
        values = set()
        for at in range(len(body) - len(pattern) + 1):
            if all((body[at + k] & mask[k]) == (pattern[k] & mask[k]) for k in range(len(pattern))):
                values.add(decode_site(site, body[at:at + len(pattern)], start + at))
        return len(values) == 1

    def site(self, name, function, predicate, kind):
        code = self.body(function)
        for i, ins in enumerate(code):
            if predicate(i, code):
                break
        else:
            self.report.append(f"{name}: site NOT FOUND in {function}")
            return None
        if kind == "Rip":
            operand, width, value = ins.disp_offset, 4, rip_target(ins)
        elif kind == "Slot":
            operand, width, value = ins.disp_offset, ins.disp_size, mem_disp(ins) // 8
        elif kind == "Imm":
            operand, width, value = ins.imm_offset, ins.imm_size, ins.operands[-1].imm
        else:  # Disp
            operand, width, value = ins.disp_offset, ins.disp_size, mem_disp(ins)
        site = self.window(function, i, operand, width)
        site.update(kind=kind, value=value, string=None)
        return self.unique_in_function(name, site)

    def call_site(self, name):
        parent, rva = self.callsite[name]
        code = self.body(parent)
        index = next(i for i, ins in enumerate(code) if ins.address == rva)
        site = self.window(parent, index, 1, 4)
        site.update(kind="Call", value=self.found[name], string=None)
        return self.unique_in_function(name, site)

    def schema_site(self, name, field):
        im = self.image
        for string_rva in im.find_string(field):
            for x in im.rip_xrefs(string_rva):
                start = im.function_start(x) or x - 0x40
                # The registration call passes the field offset in the fifth argument slot just before the name.
                candidates = [ins for ins in im.disasm(start, x - start + 8) if ins.address < x and x - ins.address <= 0x40
                              and text_of(ins).startswith("mov dword ptr [rsp + 0x28], ")]
                if candidates:
                    ins = candidates[-1]
                    return {"function": None, "offset": ins.address, "bytes": bytes(ins.bytes).hex(), "mask": "ffffffff00000000",
                            "size": ins.size, "operand": ins.imm_offset, "end": ins.size, "width": 4, "kind": "Schema",
                            "value": ins.operands[1].imm, "string": field, "index": 0}
        self.report.append(f"{name}: schema field {field} NOT FOUND")
        return None

    def image_value(self, name):
        return self.values.get(name)

    def sites(self):
        out = {}

        def S(name, function, predicate, kind):
            result = self.site(name, function, predicate, kind)
            if result:
                self.values[name] = result["value"]
            return result
        for name in FUNCTIONS:
            if name in self.callsite:
                out[name] = self.call_site(name)
        out["ModelInfo"] = S("ModelInfo", "SetModel", lambda i, c: text_of(c[i]).startswith("mov rcx, qword ptr [rip"), "Rip")
        out["FindOrLoadSlot"] = S("FindOrLoadSlot", "SetModel", lambda i, c: text_of(c[i]).startswith("call qword ptr [rax +"), "Slot")
        out["InventoryManager"] = S("InventoryManager", "EquippedView", lambda i, c: text_of(c[i]).startswith("lea r14, [rip"), "Rip")
        out["EntityChunks"] = S("EntityChunks", "DestroyWearables", lambda i, c: text_of(c[i]).startswith("mov r8, qword ptr [rip"), "Rip")
        out["WearableCount"] = S("WearableCount", "DestroyWearables", lambda i, c: c[i].mnemonic == "movsxd" and mem_disp(c[i], "rcx"), "Disp")
        out["ResourceSystem"] = S("ResourceSystem", "SetBaseModel", lambda i, c: c[i].mnemonic == "cmp" and rip_target(c[i]), "Rip")
        out["ReleaseSlot"] = S("ReleaseSlot", "SetBaseModel", lambda i, c: text_of(c[i]).startswith("call qword ptr [rax +"), "Slot")
        out["ModelRefCount"] = S("ModelRefCount", "SetBaseModel", lambda i, c: text_of(c[i]).startswith("lock dec dword ptr [rdx +"), "Disp")
        out["LocalController"] = S("LocalController", "LocalControllerGetter", lambda i, c: text_of(c[i]).startswith("lea rcx, [rip"), "Rip")
        out["Modifiers"] = S("Modifiers", "ModelOverride", lambda i, c: text_of(c[i]).startswith("mov rcx, qword ptr [rdi +"), "Disp")
        out["ModifierPath"] = S("ModifierPath", "ModelOverride", lambda i, c: text_of(c[i]).startswith("mov rax, qword ptr [rbx +"), "Disp")
        out["CreationList"] = S("CreationList", "PrepareWearables", lambda i, c: c[i].mnemonic == "movsxd" and mem_disp(c[i]) and mem_disp(c[i]) > 0x100, "Disp")
        out["CreationInitialized"] = S("CreationInitialized", "PrepareWearables", lambda i, c: text_of(c[i]).startswith("cmp byte ptr [r13 +") and text_of(c[i]).endswith(", 0"), "Disp")
        eq = self.found["EquippedView"]
        out["HeroId"] = S("HeroId", "BuildWearableList", lambda i, c: c[i].mnemonic == "mov" and "edx" in c[i].op_str and mem_disp(c[i]) and any(direct_call_target(x) == eq for x in c[i + 1:i + 4]), "Disp")
        out["PlayerId"] = S("PlayerId", "BuildWearableList", lambda i, c: c[i].mnemonic == "cmp" and c[i].op_str.endswith(", esi") and mem_disp(c[i], "r14"), "Disp")
        out["DirtyFlags"] = S("DirtyFlags", "CombineModels", lambda i, c: c[i].mnemonic == "test" and c[i].op_str.endswith(", 0x10") and mem_disp(c[i], "r14"), "Disp")
        out["BaseModel"] = S("BaseModel", "CombineModels", lambda i, c: c[i].mnemonic == "cmp" and c[i].op_str.endswith(", rdx") and mem_disp(c[i], "r14"), "Disp")
        vm = self.found["ViewModel"]
        out["ModelIndex"] = S("ModelIndex", "CombineModels", lambda i, c: c[i].mnemonic == "mov" and "edx" in c[i].op_str and mem_disp(c[i], "r14") and any(direct_call_target(x) == vm for x in c[i + 1:i + 5]), "Disp")
        jit = self.image.find_string("JustInTimeManifest")[0]
        out["JustInTimeManifest"] = S("JustInTimeManifest", "CombineModels", lambda i, c: c[i].mnemonic == "lea" and rip_target(c[i]) == jit, "Rip")
        if out["JustInTimeManifest"]:
            out["JustInTimeManifest"]["string"] = "JustInTimeManifest"  # the resolved target must still be this literal
        ctor = self.found["ResourceNameCtor"]
        out["ResourceSystemInterface"] = S("ResourceSystemInterface", "CombineModels", lambda i, c: text_of(c[i]).startswith("mov rbx, qword ptr [rip") and any(direct_call_target(x) == ctor for x in c[i - 12:i]), "Rip")
        out["ResourceStateSlot"] = S("ResourceStateSlot", "CombineModels", lambda i, c: text_of(c[i]).startswith("call qword ptr [rax +") and any(x.mnemonic == "lea" and rip_target(x) == jit for x in c[i + 1:i + 8]), "Slot")
        out["ResourceRegisterSlot"] = S("ResourceRegisterSlot", "CombineModels", lambda i, c: text_of(c[i]).startswith("call qword ptr [r9 +") and any(x.mnemonic == "lea" and rip_target(x) == jit for x in c[i - 3:i]), "Slot")
        out["ResourceFindSlot"] = S("ResourceFindSlot", "CombineModels", lambda i, c: text_of(c[i]).startswith("call qword ptr [r9 +") and any(text_of(x) == "xor r8d, r8d" for x in c[i - 2:i]) and any(x.mnemonic == "lea" and rip_target(x) == jit for x in c[i - 8:i]), "Slot")
        # tier0's g_pMemAlloc is not exported by name; the allocator thunk carries the import slot.
        out["MemAllocImport"] = S("MemAllocImport", "Allocate", lambda i, c: text_of(c[i]).startswith("mov rax, qword ptr [rip"), "Rip")
        # The engine's own name lookup pins the slot; adjacency to FindOrLoad is not assumed.
        out["ModelNameSlot"] = S("ModelNameSlot", "ModelNameLookup", lambda i, c: text_of(c[i]).startswith("call qword ptr [rax + ")
                                and any(rip_target(x) == self.image_value("ModelInfo") for x in c[max(0, i - 10):i]), "Slot")
        # The modifier list is heap allocated, so this size has to be the engine's own.
        out["ModifierStorage"] = S("ModifierStorage", "PreviewBuilder", lambda i, c: text_of(c[i]) == "mov ecx, 0x30", "Imm")
        out["SteamId"] = self.schema_site("SteamId", "m_steamID")
        out["AssignedHero"] = self.schema_site("AssignedHero", "m_hAssignedHero")
        return out


# --------------------------------------------------------------------------- output
def cpp_array(hexdata):
    return "{" + ",".join(f"0x{b:02x}" for b in bytes.fromhex(hexdata)) + "}"


def emit_header(profile, path):
    values = list(GLOBALS) + list(OFFSETS) + list(SLOTS)
    lines = ["#pragma once", "#include <cstddef>", "#include <cstdint>", "",
             "// Generated by refresh_profile.py from a verified client.dll. The values below",
             "// are hints from that build: the DLL re-locates every function by masked",
             "// signature or by the call that reaches it, and re-reads every global, struct",
             "// offset and vtable slot from the instruction that carries it, so a Dota update",
             "// that only moves code needs no rebuild. See src/native_appearance_resolver.h.",
             "namespace appearance::profile {",
             f'inline constexpr char Sha256[] = "{profile["sha256"]}";',
             f"inline constexpr uint32_t Timestamp = {profile['timestamp']}, ImageSize = {profile['image_size']};",
             "",
             "// Small ABI constants that no build has changed; a mismatch would break the",
             "// resolver's own sanity checks long before these mattered.",
             "inline constexpr uint32_t EntityIdentity = 0x10, IdentityHandle = 0x10, IdentityStride = 0x70;",
             "inline constexpr uint32_t ModifierRefCount = 8, ViewDestructorSlot = 2, ModifierDestructorSlot = 0;",
             "// CEntityKeyValues was 0x38 bytes in every build seen; the exact size is not",
             "// encoded anywhere readable, so the stack buffers holding one are oversized.",
             "inline constexpr uint32_t KeyValuesStorage = 0x100, LocalInventoryOffset = 0x120;",
             "inline constexpr uint32_t MemAllocSlot = 1, MemFreeSlot = 3;  // tier0 IMemAlloc::Alloc / ::Free",
             "inline constexpr uint32_t ModelResourceType = 0x6c646d76;  // 'vmdl'",
             "",
             "// Resolved at load time. Never treat these as constants."]
    for name in FUNCTIONS:
        lines.append(f"inline uintptr_t {name} = {profile['functions'][name]['rva']};")
    for name in values:
        lines.append(f"inline uintptr_t {name} = {profile['values'][name]:#x};")
    lines += ["",
              "enum class SiteKind : uint8_t { Rip, Disp, Slot, Imm, Call, Schema };",
              "struct Signature { const char* name; uintptr_t* target; const uint8_t* bytes; const uint8_t* mask; uint32_t size; };",
              "// bytes/mask cover a window of instructions ending with the one that carries the",
              "// value; operand is the index of that value in the window and end is the index",
              "// just past its instruction (rip- and call-relative operands are measured from there).",
              "struct Site { const char* name; uintptr_t* target; uintptr_t* function; uint32_t offset;",
              "              const uint8_t* bytes; const uint8_t* mask; uint16_t size, operand, end;",
              "              uint8_t width; SiteKind kind; bool verify; const char* text; };", ""]
    signed = [n for n in FUNCTIONS if profile["functions"][n].get("signature")]
    for name in signed:
        f = profile["functions"][name]
        lines.append(f"inline constexpr uint8_t {name}Bytes[] = {cpp_array(f['signature'])};")
        lines.append(f"inline constexpr uint8_t {name}Mask[] = {cpp_array(f['mask'])};")
    lines.append("inline constexpr Signature Functions[] = {")
    for name in signed:
        lines.append(f'    {{"{name}", &{name}, {name}Bytes, {name}Mask, sizeof({name}Bytes)}},')
    lines += ["};", ""]
    for name, site in profile["sites"].items():
        lines.append(f"inline constexpr uint8_t {name}Site[] = {cpp_array(site['bytes'])};")
        lines.append(f"inline constexpr uint8_t {name}SiteMask[] = {cpp_array(site['mask'])};")
    lines.append("inline constexpr Site Sites[] = {")
    for name, site in profile["sites"].items():
        function = f'&{site["function"]}' if site["function"] else "nullptr"
        text = f'"{site["string"]}"' if site["string"] else "nullptr"
        lines.append(f'    {{"{name}", &{name}, {function}, {site["offset"]:#x}, {name}Site, {name}SiteMask, '
                     f'{site["size"]}, {site["operand"]}, {site["end"]}, {site["width"]}, SiteKind::{site["kind"]}, '
                     f'{"true" if site.get("verify") else "false"}, {text}}},')
    lines += ["};", "}"]
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--client", type=Path, default=None)
    parser.add_argument("--profile", type=Path, default=ROOT / "data" / "native_profile.json")
    parser.add_argument("--header", type=Path, default=ROOT / "src" / "native_appearance_profile.h")
    parser.add_argument("--check", action="store_true", help="verify only; do not rewrite the profile")
    parser.add_argument("--seed", action="append", default=[], metavar="NAME=RVA", help="known address for a function without anchor (first run)")
    parser.add_argument("--accept-changes", action="store_true", help="record globals/offsets that resolved to a new value")
    args = parser.parse_args(argv)
    client = args.client
    if not client:
        import dota_vpk
        game = dota_vpk.game_directory()
        client = game / "bin" / "win64" / "client.dll" if game else None
    if not client or not Path(client).exists():
        print("[!] client.dll not found; pass --client")
        return 2
    previous = json.loads(args.profile.read_text(encoding="utf-8")) if args.profile.exists() else {}
    # Reading a hundred megabytes and indexing 200k unwind records takes a while;
    # say which file is being opened before going quiet, not after.
    print(f"[*] Lecture de {client}")
    image = Image(client)
    print(f"[*] {client}: sha256 {image.sha256[:16]}..., timestamp {image.pe.FILE_HEADER.TimeDateStamp:#x}")
    if previous.get("sha256") == image.sha256 and not args.seed:
        print("[OK] client.dll already matches the profile")
        return 0
    locator = Locator(image, previous)
    for seed in args.seed:
        name, _, rva = seed.partition("=")
        if name not in FUNCTIONS or not rva:
            print(f"[!] --seed {seed}: expected one of {', '.join(FUNCTIONS)} followed by =<rva>")
            return 2
        address = int(rva, 16)
        if address not in image.starts:
            print(f"[!] --seed {name}={rva}: {address:#x} does not begin a function in this client.dll")
            return 2
        print(f"[*] {name}: seeded at {address:#x} by hand")
        pattern, mask = unique_signature(image, address)
        previous.setdefault("functions", {})[name] = {"rva": rva, "signature": pattern.hex() if pattern else "", "mask": mask.hex() if mask else ""}
    found = locator.run()
    missing = [n for n in FUNCTIONS if n not in found]
    sites = locator.sites() if not missing else {}
    for line in locator.report:
        print("    " + line)
    unresolved = missing + [k for k, v in sites.items() if v is None]
    if unresolved:
        print(f"[!] unresolved: {unresolved}")
        return 1
    profile = {"sha256": image.sha256, "timestamp": f"{image.pe.FILE_HEADER.TimeDateStamp:#x}",
               "image_size": f"{image.pe.OPTIONAL_HEADER.SizeOfImage:#x}", "functions": {}, "sites": sites, "values": {}}
    without_signature = []
    for name in FUNCTIONS:
        pattern, mask = unique_signature(image, found[name])
        if pattern is None:
            without_signature.append(name)
            if name not in locator.callsite:
                print(f"[!] {name}: no unique signature and no call site")
                return 1
        profile["functions"][name] = {"rva": f"{found[name]:#x}", "signature": pattern.hex() if pattern else "", "mask": mask.hex() if mask else ""}
    for name, site in sites.items():
        if site["kind"] != "Call":
            profile["values"][name] = site["value"]
        # Where a function already has a signature unique in .text, the weaker
        # anchor must not silently win: the DLL compares the two instead.
        site["verify"] = site["kind"] == "Call" and bool(profile["functions"][name]["signature"])
    for name in GLOBALS + OFFSETS + SLOTS:
        if name not in profile["values"]:
            print(f"[!] {name}: no site produced a value")
            return 1
    for name, site in sites.items():
        base = 0 if site["kind"] == "Schema" else found[site["function"]]
        window = image.read(base + site["offset"], site["size"])
        if bytes(a & m for a, m in zip(window, bytes.fromhex(site["mask"]))) != bytes(a & m for a, m in zip(bytes.fromhex(site["bytes"]), bytes.fromhex(site["mask"]))):
            print(f"[!] {name}: recorded window does not match the image")
            return 1
        if decode_site(site, window, base + site["offset"]) != site["value"]:
            print(f"[!] {name}: window decodes to a different value than the disassembler read")
            return 1
    moved = {n: (previous.get("functions", {}).get(n, {}).get("rva"), f"{found[n]:#x}") for n in FUNCTIONS
             if previous.get("functions", {}).get(n, {}).get("rva") not in (None, f"{found[n]:#x}")}
    changed = {n: (previous.get("values", {}).get(n), v) for n, v in profile["values"].items() if previous.get("values", {}).get(n) not in (None, v)}
    # A global is an address in this image, so it moves whenever the build does.
    # A struct offset or a vtable slot is an ABI fact, and a new value there means
    # something else entirely; one line holding both hid the difference.
    abi = [n for n in changed if n not in GLOBALS]
    print(f"[*] functions moved: {len(moved)}; values changed: {len(changed) or 'none'}; located by call site only: {without_signature or 'none'}")
    for name, (old, new) in changed.items():
        kind = "global" if name in GLOBALS else "struct offset" if name in OFFSETS else "vtable slot"
        print(f"    {name}: {old:#x} -> {new:#x} ({new - old:+#x}, {kind})")
    if args.check:
        # --check records nothing, and the shipped library re-reads every one of
        # these from the image at load, so a value that moved is a report rather
        # than a failure. Only the ABI kind is worth stopping a verification for.
        if abi:
            print(f"[!] every entry resolves, but a struct offset or vtable slot changed: {', '.join(abi)}.")
            print("    A mis-picked anchor looks exactly like a real Dota change here. Check each one before")
            print("    regenerating the profile.")
            return 1
        print("[OK] every entry resolves in this client.dll (run without --check to record it)")
        return 0
    if changed and not args.accept_changes:
        if abi:
            print(f"[!] a struct offset or vtable slot resolved to a different value than the recorded one: {', '.join(abi)}.")
            print("    That is either a real Dota change or a mis-picked anchor, and the second is not safe to")
            print("    ship. Check each one, then re-run with --accept-changes to record them.")
        else:
            print("[!] the globals above moved with the image, which is what a Dota update does to every one of")
            print("    them; no struct offset and no vtable slot changed. Re-run with --accept-changes to record")
            print("    them.")
        return 1
    args.profile.parent.mkdir(parents=True, exist_ok=True)
    args.profile.write_text(json.dumps(profile, indent=1), encoding="utf-8")
    emit_header(profile, args.header)
    print(f"[OK] wrote {args.profile} and {args.header}; run build.bat")
    return 0


if __name__ == "__main__":
    sys.exit(main())
