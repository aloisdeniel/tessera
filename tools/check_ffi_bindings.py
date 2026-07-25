#!/usr/bin/env python3
"""check_ffi_bindings.py — FFI binding drift gate.

Diffs the struct layouts declared in bindings/lua/tessera.lua (LuaJIT cdef)
and bindings/dart/lib/src/ffi.dart (dart:ffi) against the canonical layout
emitted by the C compiler (`test_ffi_layout --dump`). Any mismatch — missing
struct, missing/extra field, wrong offset, wrong size — is a hard failure.

The bindings never carry explicit offsets, so this script recomputes each
declared struct's layout with the target ABI rules (LP64, natural alignment)
and compares field-by-field with what the C compiler actually produced.

Usage:
  check_ffi_bindings.py --dump-tool <path-to-test_ffi_layout> \
                        --lua <tessera.lua> --dart <ffi.dart>
"""
import argparse
import re
import subprocess
import sys

# ---- target ABI primitives (LP64) -------------------------------------------
PRIM = {
    "bool": (1, 1), "_Bool": (1, 1),
    "char": (1, 1), "int8_t": (1, 1), "uint8_t": (1, 1),
    "int16_t": (2, 2), "uint16_t": (2, 2),
    "int": (4, 4), "int32_t": (4, 4), "uint32_t": (4, 4),
    "float": (4, 4),
    "double": (8, 8),
    "int64_t": (8, 8), "uint64_t": (8, 8), "size_t": (8, 8),
}
PTR = (8, 8)
ENUM = (4, 4)


def align_up(n, a):
    return (n + a - 1) & ~(a - 1)


class Layout:
    """Computed layout of one declared struct: [(name, offset, size)], size."""

    def __init__(self, name):
        self.name = name
        self.fields = []   # (field_name, offset, size)
        self.size = 0
        self.align = 1

    def add(self, fname, fsize, falign):
        off = align_up(self.size, falign)
        self.fields.append((fname, off, fsize))
        self.size = off + fsize
        self.align = max(self.align, falign)

    def close(self):
        self.size = align_up(self.size, self.align)


# ---- canonical dump ----------------------------------------------------------
def parse_dump(text):
    """-> ({struct: {'size','align','fields':[(name,off,size)]}}, {scalar: size})"""
    structs, scalars, order = {}, {}, []
    for line in text.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "struct":
            structs[parts[1]] = {
                "size": int(parts[2].split("=")[1]),
                "align": int(parts[3].split("=")[1]),
                "fields": [],
            }
            order.append(parts[1])
        elif parts[0] == "field":
            structs[parts[1]]["fields"].append(
                (parts[2], int(parts[3].split("=")[1]), int(parts[4].split("=")[1])))
        elif parts[0] == "scalar":
            scalars[parts[1]] = int(parts[2].split("=")[1])
    return structs, scalars, order


# ---- Lua cdef parser ---------------------------------------------------------
def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def parse_lua(path):
    """Parse the ffi.cdef[[...]] block; -> {struct_name: Layout}."""
    src = open(path).read()
    m = re.search(r"ffi\.cdef\s*\[\[(.*?)\]\]", src, flags=re.S)
    if not m:
        sys.exit(f"error: no ffi.cdef[[...]] block found in {path}")
    cdef = strip_comments(m.group(1))

    aliases = {}   # name -> (size, align) for scalar typedefs / enums / fn ptrs
    structs = {}   # name -> Layout

    for em in re.finditer(r"typedef\s+enum\s*\{.*?\}\s*(\w+)\s*;", cdef, flags=re.S):
        aliases[em.group(1)] = ENUM
    for fm in re.finditer(r"typedef\s+[\w\s\*]+\(\s*\*\s*(\w+)\s*\)\s*\([^;]*\)\s*;", cdef):
        aliases[fm.group(1)] = PTR
    for am in re.finditer(r"typedef\s+(\w+)\s+(\w+)\s*;", cdef):
        base, name = am.groups()
        if base in ("struct", "enum", "union"):
            continue
        if base in PRIM:
            aliases[name] = PRIM[base]

    def field_type(base, decl):
        """(size, align, resolved_kind) for one declarator against a base type."""
        if "*" in decl:
            return PTR
        if base in PRIM:
            return PRIM[base]
        if base in aliases:
            return aliases[base]
        if base in structs:
            st = structs[base]
            return (st.size, st.align)
        sys.exit(f"error: {path}: unknown type '{base}' in cdef")

    for sm in re.finditer(r"typedef\s+struct\s*\{(.*?)\}\s*(\w+)\s*;", cdef, flags=re.S):
        body, name = sm.groups()
        lay = Layout(name)
        for decl in body.split(";"):
            decl = decl.strip()
            if not decl:
                continue
            decl = re.sub(r"\bconst\b", "", decl).strip()
            tm = re.match(r"^(\w+)\s*(.*)$", decl, flags=re.S)
            base, rest = tm.group(1), tm.group(2)
            for d in rest.split(","):
                d = d.strip()
                arr = re.search(r"\[(\d+)\]", d)
                n = int(arr.group(1)) if arr else 1
                fname = re.sub(r"\[.*\]", "", d).replace("*", "").strip()
                fsize, falign = field_type(base, d)
                lay.add(fname, fsize * n, falign)
        lay.close()
        structs[name] = lay
    return structs


# ---- Dart dart:ffi parser ----------------------------------------------------
DART_ANN = {
    "Int8": (1, 1), "Uint8": (1, 1),
    "Int16": (2, 2), "Uint16": (2, 2),
    "Int32": (4, 4), "Uint32": (4, 4),
    "Int64": (8, 8), "Uint64": (8, 8),
    "Float": (4, 4), "Double": (8, 8),
    "Bool": (1, 1),
    "Size": (8, 8), "IntPtr": (8, 8), "UintPtr": (8, 8),
}


def parse_dart(path):
    """Parse `final class X extends Struct { ... }` blocks; -> {name: Layout}."""
    src = strip_comments(open(path).read())
    structs = {}
    for cm in re.finditer(
            r"final\s+class\s+(\w+)\s+extends\s+Struct\s*\{(.*?)\n\}", src, flags=re.S):
        name, body = cm.groups()
        lay = Layout(name)
        for fm in re.finditer(
                r"(?:@(\w+)\(\s*(\d*)\s*\)\s*)?external\s+([\w<>,\s]+?)\s+(\w+)\s*;",
                body):
            ann, ann_arg, ftype, fname = fm.groups()
            ftype = ftype.strip()
            if ann == "Array":
                em = re.match(r"Array<(\w+)>", ftype)
                if not em or em.group(1) not in DART_ANN:
                    sys.exit(f"error: {path}: unsupported array type "
                             f"'{ftype}' in {name}.{fname}")
                esize, ealign = DART_ANN[em.group(1)]
                lay.add(fname, esize * int(ann_arg), ealign)
            elif ann in DART_ANN:
                lay.add(fname, *DART_ANN[ann])
            elif ann is not None:
                sys.exit(f"error: {path}: unsupported annotation "
                         f"'@{ann}' on {name}.{fname}")
            elif ftype.startswith("Pointer<"):
                lay.add(fname, *PTR)
            elif ftype in structs:
                st = structs[ftype]
                lay.add(fname, st.size, st.align)
            else:
                sys.exit(f"error: {path}: unannotated field {name}.{fname} of "
                         f"unknown type '{ftype}' (declare structs before use)")
        lay.close()
        structs[name] = lay
    return structs


# ---- comparison --------------------------------------------------------------
def snake_to_camel(s):
    return re.sub(r"_(\w)", lambda m: m.group(1).upper(), s)


def check_binding(label, canon, order, declared, rename=lambda n: n):
    errors = []
    for sname in order:
        c = canon[sname]
        if sname not in declared:
            errors.append(f"{label}: struct {sname} is not declared")
            continue
        d = declared[sname]
        cfields = c["fields"]
        want = {rename(fn): (off, sz) for (fn, off, sz) in cfields}
        got = {fn: (off, sz) for (fn, off, sz) in d.fields}
        for fn, off, sz in cfields:
            rn = rename(fn)
            if rn not in got:
                errors.append(f"{label}: {sname}.{rn} missing (C ABI: off={off} size={sz})")
                continue
            goff, gsz = got[rn]
            if goff != off:
                errors.append(f"{label}: {sname}.{rn} offset {goff} != C ABI {off}")
            if gsz != sz:
                errors.append(f"{label}: {sname}.{rn} size {gsz} != C ABI {sz}")
        for fn in got:
            if fn not in want:
                errors.append(f"{label}: {sname}.{fn} is not a field of the C struct")
        if d.size != c["size"]:
            errors.append(f"{label}: sizeof({sname}) = {d.size} != C ABI {c['size']}")
    return errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump-tool", required=True,
                    help="path to the built test_ffi_layout binary")
    ap.add_argument("--lua", required=True, help="path to bindings/lua/tessera.lua")
    ap.add_argument("--dart", required=True, help="path to bindings/dart/lib/src/ffi.dart")
    args = ap.parse_args()

    dump = subprocess.run([args.dump_tool, "--dump"], capture_output=True,
                          text=True, check=True).stdout
    canon, _scalars, order = parse_dump(dump)
    if not canon:
        sys.exit("error: empty layout dump — is the dump tool built correctly?")

    lua = parse_lua(args.lua)
    dart = parse_dart(args.dart)

    errors = []
    errors += check_binding("lua", canon, order, lua)
    errors += check_binding("dart", canon, order, dart, rename=snake_to_camel)

    if errors:
        print(f"FFI binding drift detected ({len(errors)} error(s)):")
        for e in errors:
            print(f"  {e}")
        sys.exit(1)
    nfields = sum(len(c["fields"]) for c in canon.values())
    print(f"ok: {len(canon)} structs / {nfields} fields match the C ABI "
          f"in both bindings (lua, dart)")


if __name__ == "__main__":
    main()
