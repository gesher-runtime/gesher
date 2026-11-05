"""Small classfile/DEX fixtures exercise actual encodings and conservative lookup."""
from pathlib import Path
from dataclasses import replace
import hashlib
import struct
import sys
import tempfile
import unittest
import zipfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import java_member_inventory as audit


def uleb(value):
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    return bytes(result + bytes([value]))


def classfile(name="sample/AChild", parent="sample/ZParent", members=(), interfaces=()):
    cp = []
    def text(value):
        value = value.encode("utf-8")
        cp.append(b"\x01" + struct.pack(">H", len(value)) + value)
        return len(cp)
    def cls(value):
        index = text(value)
        cp.append(b"\x07" + struct.pack(">H", index))
        return len(cp)
    this, sup = cls(name), cls(parent) if parent else 0
    parents = [cls(value) for value in interfaces]
    fields, methods = [], []
    for kind, member, desc, flags in members:
        target = fields if kind == "field" else methods
        target.append(struct.pack(">HHHH", flags, text(member), text(desc), 0))
    return (b"\xca\xfe\xba\xbe" + struct.pack(">HHH", 0, 52, len(cp) + 1) + b"".join(cp)
            + struct.pack(">HHHH", 1, this, sup, len(parents))
            + b"".join(struct.pack(">H", value) for value in parents)
            + struct.pack(">H", len(fields)) + b"".join(fields)
            + struct.pack(">H", len(methods)) + b"".join(methods) + b"\x00\x00")


def fix_dex(data):
    struct.pack_into("<I", data, 32, len(data))
    data[12:32] = hashlib.sha1(data[32:]).digest()
    struct.pack_into("<I", data, 8, zlib.adler32(data[12:]) & 0xffffffff)
    return bytes(data)


def dex(defs):
    """defs=(name,parent,interfaces,flags, [(kind,name,descriptor,flags),...])."""
    def desc(name):
        return "L" + name.replace(".", "/") + ";"
    types = {desc(name) for name, *_ in defs}
    types.update(desc(parent) for _, parent, *_ in defs if parent)
    types.update(desc(iface) for _, _, ifaces, *_ in defs for iface in ifaces)
    protos = set()
    for _, _, _, _, members in defs:
        for kind, _, signature, _ in members:
            if kind == "method":
                args, result = signature[1:].split(")")
                # Fixture methods use one-character primitive argument types.
                protos.add((tuple(args), result))
                types.update(args)
                types.add(result)
            else:
                types.add(signature)
    types, protos = sorted(types), sorted(protos)
    shorty = lambda proto: proto[1] + "".join(proto[0])
    strings = sorted(set(types) | {shorty(proto) for proto in protos} |
                     {member[1] for *_, members in defs for member in members})
    si, ti = {v:i for i,v in enumerate(strings)}, {v:i for i,v in enumerate(types)}
    pi = {"(" + "".join(args) + ")" + result:i for i,(args,result) in enumerate(protos)}
    fields, methods = [], []
    for name, _, _, _, members in defs:
        for kind, member, signature, flags in members:
            (fields if kind == "field" else methods).append((ti[desc(name)], member, signature, flags))
    fields.sort(key=lambda v:(v[0], si[v[1]], ti[v[2]]))
    methods.sort(key=lambda v:(v[0], si[v[1]], pi[v[2]]))
    positions, at = {}, 112
    for name, values, width in (("strings", strings, 4), ("types", types, 4),
                                 ("protos", protos, 12), ("fields", fields, 8),
                                 ("methods", methods, 8), ("classes", defs, 32)):
        positions[name] = at if values else 0
        at += len(values)*width
    data_at = at
    data = bytearray(at)
    data[:8] = b"dex\n039\0"
    struct.pack_into("<II", data, 36, 112, 0x12345678)
    for offset, name, values in ((56,"strings",strings), (64,"types",types), (72,"protos",protos),
                                 (80,"fields",fields), (88,"methods",methods), (96,"classes",defs)):
        struct.pack_into("<II", data, offset, len(values), positions[name])
    for i, value in enumerate(strings):
        struct.pack_into("<I", data, positions["strings"] + i*4, len(data))
        data += uleb(len(value)) + value.encode("utf-8") + b"\0"
    for i, value in enumerate(types):
        struct.pack_into("<I", data, positions["types"] + i*4, si[value])
    def align():
        while len(data) % 4:
            data.extend(b"\0")
    def typelist(values):
        if not values:
            return 0
        align()
        position = len(data)
        data.extend(struct.pack("<I", len(values)) + b"".join(struct.pack("<H", value) for value in values))
        return position
    for i, proto in enumerate(protos):
        params = typelist([ti[value] for value in proto[0]])
        struct.pack_into("<III", data, positions["protos"] + i*12, si[shorty(proto)], ti[proto[1]], params)
    for name, values in (("fields", fields), ("methods", methods)):
        for i,(owner, member, signature, _) in enumerate(values):
            kind_index = ti[signature] if name == "fields" else pi[signature]
            struct.pack_into("<HHI", data, positions[name] + i*8, owner, kind_index, si[member])
    for i,(name, parent, ifaces, flags, _) in enumerate(defs):
        own = ti[desc(name)]
        iface_at = typelist([ti[desc(value)] for value in ifaces])
        groups = [[], [], [], []]
        for index, item in enumerate(fields):
            if item[0] == own:
                groups[0 if item[3] & audit.STATIC else 1].append((index, item[3], None))
        for index, item in enumerate(methods):
            if item[0] != own:
                continue
            flags_ = item[3]
            code = 0
            if not flags_ & (audit.NATIVE | audit.ABSTRACT):
                align()
                code = len(data)
                data.extend(b"\0"*16)
            direct = bool(flags_ & (audit.PRIVATE | audit.STATIC)) or item[1] in ("<init>", "<clinit>")
            groups[2 if direct else 3].append((index, flags_, code))
        class_data = len(data)
        for group in groups:
            data.extend(uleb(len(group)))
        for group in groups:
            prev = 0
            for index, flags_, code in group:
                data.extend(uleb(index-prev) + uleb(flags_))
                prev = index
                if code is not None:
                    data.extend(uleb(code))
        struct.pack_into("<IIIIIIII", data, positions["classes"] + i*32, own, flags,
                         ti[desc(parent)] if parent else 0xffffffff, iface_at, 0xffffffff, 0, class_data, 0)
    align()
    map_at = len(data)
    data.extend(struct.pack("<IHHII", 1, 0x1000, 0, 1, map_at))
    struct.pack_into("<I", data, 52, map_at)
    struct.pack_into("<II", data, 104, len(data)-data_at, data_at)
    return fix_dex(data)


def model(name, members=(), parent=None, ifaces=(), flags=1):
    members = [audit.Member(*item) for item in members]
    return audit.Class(name, flags, parent, tuple(ifaces), {item.key:item for item in members})


class EncodingTests(unittest.TestCase):
    def test_modified_utf8_nul_supplementary_and_length(self):
        self.assertEqual(audit.modified_utf8(b"x\xc0\x80\xed\xa0\xbd\xed\xb8\x80", 4), "x\0\U0001f600")
        for raw in (b"\xc0", b"\xc1\x81", b"\xf0\x90\x80\x80", b"\0"):
            with self.assertRaises(audit.InvalidInput):
                audit.modified_utf8(raw)
        with self.assertRaisesRegex(audit.InvalidInput, "length"):
            audit.modified_utf8(b"x", 2)

    def test_descriptor_overloads_and_void(self):
        for signature, kind in (("I", "field"), ("[[Ljava/lang/String;", "field"), ("(I[J)V", "method")):
            self.assertEqual(audit.descriptor(signature, kind), signature)
        for signature, kind in (("V", "field"), ("[V", "field"), ("(V)V", "method"), ("()VV", "method"), ("(I)", "method")):
            with self.assertRaises(audit.InvalidInput):
                audit.descriptor(signature, kind)

    def test_classfile_members_and_parents(self):
        cls = audit.class_members(classfile(members=(("field","VALUE","I",9), ("method","f","(I)V",5)), interfaces=("sample/Face",)))
        self.assertEqual(cls.superclass, "sample.ZParent")
        self.assertEqual(cls.interfaces, ("sample.Face",))
        self.assertEqual(cls.members[("method","f","(I)V")].flags, 5)

    def test_classfile_truncated_attributes_rejected(self):
        raw = classfile(members=(("method","f","()V",1),))
        for end in (10, len(raw)-1, len(raw)-4):
            with self.assertRaises(audit.InvalidInput):
                audit.class_members(raw[:end])

    def test_long_method_descriptor_has_separate_explicit_bound(self):
        signature = "(" + "Ljava/lang/String;"*80 + ")V"
        raw = classfile(members=(("method","f",signature,1),))
        self.assertIn(("method","f",signature),audit.class_members(raw).members)
        with self.assertRaisesRegex(audit.InvalidInput,"member_string"):
            audit.class_members(raw,replace(audit.Limits(),max_member_descriptor_bytes=1000))

    def test_dex_dependency_order_delta_reset_and_definitions_only(self):
        raw = dex([("sample.ZParent",None,(),1,(("field","a","I",9),("field","b","I",1),
                                                  ("method","<init>","()V",1),("method","f","(I)V",1),
                                                  ("method","f","()V",1))),
                   ("sample.AChild","sample.ZParent",("sample.External",),1,())])
        classes = audit.dex_members(raw)
        self.assertEqual(set(classes), {"sample.ZParent","sample.AChild"})
        self.assertEqual(len(classes["sample.ZParent"].members), 5)
        self.assertEqual(classes["sample.AChild"].superclass, "sample.ZParent")
        self.assertEqual(classes["sample.AChild"].interfaces, ("sample.External",))

    def test_dex_truncated_class_data_rejected(self):
        raw = bytearray(dex([("sample.A",None,(),1,(("method","f","()V",1),))]))
        class_at = struct.unpack_from("<I", raw, 100)[0]
        struct.pack_into("<I", raw, class_at+24, len(raw)-1)
        raw[-1] = 0x80
        with self.assertRaises(audit.InvalidInput):
            audit.dex_members(fix_dex(raw))

    def test_dex_wrong_member_owner_rejected(self):
        raw = bytearray(dex([("sample.A",None,(),1,(("method","f","()V",1),)), ("sample.B",None,(),1,())]))
        method_at = struct.unpack_from("<I", raw, 92)[0]
        struct.pack_into("<H", raw, method_at, 1)
        with self.assertRaisesRegex(audit.InvalidInput, "owner"):
            audit.dex_members(fix_dex(raw))

    def test_array_method_reference_is_not_array_class_definition(self):
        raw = bytearray(dex([("sample.A",None,(),1,(("field","array","[I",1),("method","clone","()V",1)))]))
        type_count, type_at = struct.unpack_from("<II",raw,64)
        string_at = struct.unpack_from("<I",raw,60)[0]
        array_index = None
        for index in range(type_count):
            string_index = struct.unpack_from("<I",raw,type_at+index*4)[0]
            offset = struct.unpack_from("<I",raw,string_at+string_index*4)[0]
            if raw[offset:offset+4] == b"\x02[I\0":
                array_index = index
        self.assertIsNotNone(array_index)
        method_at = struct.unpack_from("<I",raw,92)[0]
        struct.pack_into("<H",raw,method_at,array_index)
        class_at = struct.unpack_from("<I",raw,100)[0]
        class_data = struct.unpack_from("<I",raw,class_at+24)[0]
        raw[class_data+3] = 0  # Remove its encoded definition, retain method_id reference.
        classes = audit.dex_members(fix_dex(raw))
        self.assertEqual(set(classes),{"sample.A"})
        self.assertEqual(len(classes["sample.A"].members),1)


class ResolutionTests(unittest.TestCase):
    def test_overload_and_constructor_not_inherited(self):
        parent = model("P", (("method","f","(I)V",1),("method","<init>","()V",1)))
        child = model("C", parent="P")
        resolver = audit.Resolver({"P":parent, "C":child})
        self.assertEqual(resolver.lookup("C", ("method","f","(I)V"))[0][0][0], "P")
        self.assertEqual(resolver.lookup("C", ("method","f","()V")), ([],False))
        self.assertEqual(resolver.lookup("C", ("method","<init>","()V")), ([],False))

    def test_field_interfaces_before_superclass(self):
        iface = model("I", (("field","f","I",9),), flags=0x201)
        parent = model("P", (("field","f","I",1),))
        child = model("C", parent="P", ifaces=("I",))
        self.assertEqual(audit.Resolver({"I":iface,"P":parent,"C":child}).lookup("C",("field","f","I"))[0][0][0], "I")

    def test_interface_private_and_static_not_inherited(self):
        iface = model("I", (("method","f","()V",9),("method","p","()V",2), ("method","a","()V",0x401)), flags=0x201)
        child = model("C", ifaces=("I",))
        resolver = audit.Resolver({"I":iface,"C":child})
        self.assertEqual(resolver.lookup("C",("method","f","()V")), ([],False))
        self.assertEqual(resolver.lookup("C",("method","p","()V")), ([],False))
        self.assertTrue(resolver.lookup("C",("method","a","()V"))[0])

    def test_maximal_specific_interface_and_uncertain_tie(self):
        base = model("I", (("method","f","()V",1),), flags=0x201)
        sub = model("J", (("method","f","()V",1),), ifaces=("I",), flags=0x201)
        other = model("K", (("method","f","()V",1),), flags=0x201)
        child = model("C", ifaces=("I","J"))
        classes = {value.name:value for value in (base,sub,other,child)}
        self.assertEqual(audit.Resolver(classes).lookup("C",("method","f","()V"))[0][0][0], "J")
        child.interfaces = ("I","K")
        self.assertEqual(len(audit.Resolver(classes).lookup("C",("method","f","()V"))[0]), 2)

    def test_unknown_hierarchy_prevents_absent_claim(self):
        child = model("C", parent="Absent")
        item = audit.Member("method","f","()V",1)
        result = audit.compare_member(child,item,{"C":child},audit.Resolver({"C":child}),{"C"})
        self.assertEqual(result["status"], "unresolved_hierarchy")
        self.assertEqual(result["unresolved_parent_count"], 1)

    def test_absent_class_separate_from_absent_member(self):
        cls = model("C")
        item = audit.Member("field","f","I",1)
        self.assertEqual(audit.compare_member(cls,item,{},audit.Resolver({}),{"C"}), {"status":"declaring_class_absent"})
        self.assertEqual(audit.compare_member(cls,item,{"C":cls},audit.Resolver({"C":cls}),{"C"})["status"], "absent_from_present_class")

    def test_visibility_static_final_and_abstract_flags(self):
        expected = audit.Member("method","f","()V",1)
        actual = audit.Member("method","f","()V",0x41a)
        self.assertEqual(set(audit.flag_issues(expected,actual)), {"static_instance_mismatch","public_visibility_reduced","new_final_restriction","concrete_became_abstract"})
        self.assertEqual(audit.flag_issues(audit.Member("field","x","I",4), audit.Member("field","x","I",1)), [])

    def test_covariant_descriptor_not_equated(self):
        cls = model("C", (("method","f","()Ljava/lang/String;",1),))
        expected = audit.Member("method","f","()Ljava/lang/Object;",1)
        row = audit.compare_member(cls,expected,{"C":cls},audit.Resolver({"C":cls}),{"C"})
        self.assertEqual(row["status"], "absent_from_present_class")
        self.assertEqual(row["other_descriptor_declarations_with_same_name"], 1)

    def test_cyclic_hierarchy_rejected(self):
        classes = {"A":model("A",parent="B"),"B":model("B",parent="A")}
        with self.assertRaisesRegex(audit.InvalidInput,"cyclic"):
            audit.Resolver(classes).lookup("A",("method","f","()V"))

    def test_archive_roundtrip_hashes_and_summary(self):
        members = (("field","VALUE","I",9),("method","f","()V",1),("method","f","(I)V",1))
        with tempfile.TemporaryDirectory() as tmp:
            sdk, boot = Path(tmp)/"android.jar", Path(tmp)/"framework.jar"
            with zipfile.ZipFile(sdk,"w") as archive:
                archive.writestr("sample/AChild.class", classfile(parent=None,members=members))
            with zipfile.ZipFile(boot,"w") as archive:
                archive.writestr("classes.dex", dex([("sample.AChild",None,(),1,members[:2])]))
            report, rows = audit.inventory(sdk,[boot])
            self.assertEqual(report["sdk_public_protected_member_declarations"],3)
            self.assertEqual(report["comparisons"]["installed"]["status_counts"], {"absent_from_present_class":1,"exact_descriptor_present":2})
            self.assertEqual(report["inputs"][0]["sha256"],hashlib.sha256(sdk.read_bytes()).hexdigest())
            self.assertNotIn(tmp, str(report))
            self.assertEqual(len(rows),3)

    def test_archive_member_size_bound_is_enforced(self):
        with tempfile.TemporaryDirectory() as tmp:
            sdk = Path(tmp)/"android.jar"
            with zipfile.ZipFile(sdk,"w") as archive:
                archive.writestr("sample/AChild.class",classfile())
            with self.assertRaisesRegex(audit.InvalidInput,"size_or_compression"):
                audit.archive_members(sdk,"sdk","synthetic",replace(audit.Limits(),max_entry_bytes=16))

    def test_candidate_replacement_preserves_first_provider_precedence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            sdk, first, last, candidate = [root/name for name in ("android.jar","framework.jar","gesher-app.jar","candidate.jar")]
            member = (("method","f","()V",1),)
            with zipfile.ZipFile(sdk,"w") as archive:
                archive.writestr("sample/AChild.class",classfile(parent=None,members=member))
            for path,members in ((first,()),(last,member),(candidate,member)):
                with zipfile.ZipFile(path,"w") as archive:
                    archive.writestr("classes.dex",dex([("sample.AChild",None,(),1,members)]))
            report,rows = audit.inventory(sdk,[first,last],{"gesher-app.jar":candidate})
            self.assertEqual(rows[0]["candidate"]["status"],"absent_from_present_class")
            self.assertEqual(rows[0]["installed"],rows[0]["candidate"])
            groups = report["comparisons"]["candidate"]["duplicate_provider_groups"]
            self.assertEqual(groups,[{"selected_source_id":"boot-01","skipped_source_id":"candidate-02","class_definitions":1}])

    def test_output_cannot_overwrite_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            outdir=Path(tmp)
            with self.assertRaisesRegex(audit.InvalidInput,"overwrite_input"):
                audit.safe_output_targets(outdir,[outdir/"java-member-summary.json"])


if __name__ == "__main__":
    unittest.main()
