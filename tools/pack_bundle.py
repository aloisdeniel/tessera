#!/usr/bin/env python3
"""pack_bundle.py — pack files into a Tessera "TSAB" v1 asset bundle.

The bundle is the little-endian named-blob container consumed by
tessera_lua_load_bundle (see docs/lua.md): entries become available to
embedded Lua game scripts as `tessera.asset("name")`.

Format (all integers little-endian):
  u32 magic   = 0x42415354  (bytes 'T','S','A','B')
  u32 version = 1
  u32 count
  count entries:
    u16 name_len,  name bytes (UTF-8, no NUL),
    u32 blob_len,  blob bytes

Usage:
  pack_bundle.py out.tsb file1 [file2 ...]

Each input may be a file (entry name = its basename) or a directory (every
regular file under it is added, entry name = its path relative to that
directory, '/'-separated). A later input with the same entry name replaces
the earlier one, with a warning — mirroring the loader's override semantics.

Pure stdlib; no third-party dependencies.
"""
import os
import struct
import sys

TSAB_MAGIC = 0x42415354
TSAB_VERSION = 1


def gather(inputs):
    """Resolve the input paths to an ordered {entry name: file path} map."""
    entries = {}

    def add(name, path):
        if name in entries:
            print(f"warning: entry '{name}' overridden by {path}", file=sys.stderr)
        entries[name] = path

    for arg in inputs:
        if os.path.isdir(arg):
            for root, dirs, files in os.walk(arg):
                dirs.sort()
                for fn in sorted(files):
                    path = os.path.join(root, fn)
                    rel = os.path.relpath(path, arg)
                    add(rel.replace(os.sep, "/"), path)
        elif os.path.isfile(arg):
            add(os.path.basename(arg), arg)
        else:
            sys.exit(f"error: no such file or directory: {arg}")
    if not entries:
        sys.exit("error: nothing to pack")
    return entries


def pack(entries):
    """Serialize {entry name: file path} into TSAB v1 bytes."""
    out = [struct.pack("<III", TSAB_MAGIC, TSAB_VERSION, len(entries))]
    for name, path in entries.items():
        try:
            name_bytes = name.encode("utf-8")
        except UnicodeEncodeError:
            # os.walk decodes POSIX filenames with surrogateescape; a non-UTF-8
            # on-disk name reaches here as lone surrogates. TSAB mandates UTF-8.
            sys.exit(
                f"error: entry name is not valid UTF-8"
                f" (TSAB requires UTF-8 names): {path!r}")
        if not 0 < len(name_bytes) <= 0xFFFF:
            sys.exit(f"error: entry name too long: {name}")
        with open(path, "rb") as f:
            blob = f.read()
        if len(blob) > 0xFFFFFFFF:
            sys.exit(f"error: blob too large: {path}")
        out.append(struct.pack("<H", len(name_bytes)))
        out.append(name_bytes)
        out.append(struct.pack("<I", len(blob)))
        out.append(blob)
    return b"".join(out)


def main(argv):
    if len(argv) < 3:
        sys.exit(f"usage: {os.path.basename(argv[0])} out.tsb file-or-dir [...]")
    out_path, inputs = argv[1], argv[2:]
    entries = gather(inputs)
    data = pack(entries)
    with open(out_path, "wb") as f:
        f.write(data)
    print(f"{out_path}: {len(entries)} entries, {len(data)} bytes")
    for name, path in entries.items():
        print(f"  {name}  ({os.path.getsize(path)} bytes)")


if __name__ == "__main__":
    main(sys.argv)
