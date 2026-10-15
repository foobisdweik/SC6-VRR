"""Dump (and optionally patch) a cooked UE 4.17 DataTable of simple properties.

Usage:
  python3 -I dt_dump.py <asset.uasset>                      # dump rows
  python3 -I dt_dump.py <asset.uasset> --set Prop=Value ... # patch every row's Int/Bool property

Only IntProperty, BoolProperty, and ObjectProperty fields are decoded, which is
what SC6's DB_AdvertiseInfo uses. Patching rewrites values in place (same size),
so the .uasset header stays valid. The .uexp file next to the .uasset holds the data.
"""
import struct
import sys


def read_fstring(b, o):
    n = struct.unpack_from("<i", b, o)[0]
    o += 4
    if n >= 0:
        s = b[o:o + n].rstrip(b"\0").decode("latin-1")
        return s, o + n
    s = b[o:o - 2 * n].decode("utf-16-le").rstrip("\0")
    return s, o - 2 * n


def name_map(uasset):
    b = open(uasset, "rb").read()
    o = 4  # tag
    legacy = struct.unpack_from("<i", b, o)[0]; o += 4
    if legacy != -4:
        o += 4  # LegacyUE3Version
    o += 8  # FileVersionUE4, FileVersionLicenseeUE4
    ncv = struct.unpack_from("<i", b, o)[0]; o += 4
    o += ncv * 20  # custom versions: GUID + int
    o += 4  # TotalHeaderSize
    _, o = read_fstring(b, o)  # FolderName
    o += 4  # PackageFlags
    count, offset = struct.unpack_from("<ii", b, o)
    names, p = [], offset
    for _ in range(count):
        s, p = read_fstring(b, p)
        p += 4  # name hashes (VER_UE4_NAME_HASHES_SERIALIZED)
        names.append(s)
    return names


def parse(uexp, names):
    """Yield (row_name, [(prop, type, value, value_offset, size)]) for each row."""
    b = open(uexp, "rb").read()
    def name_at(o):
        i, num = struct.unpack_from("<ii", b, o)
        n = names[i]
        return (n if num == 0 else f"{n}_{num - 1}"), o + 8

    def read_props(o):
        props = []
        while True:
            pname, o = name_at(o)
            if pname == "None":
                return props, o
            ptype, o = name_at(o)
            size, _idx = struct.unpack_from("<ii", b, o); o += 8
            if ptype == "BoolProperty":
                val, voff = b[o], o
                o += 1
                o += 1  # HasPropertyGuid
                props.append((pname, ptype, bool(val), voff, 1))
                continue
            if ptype in ("StructProperty",):
                o += 8 + 16  # struct name + struct guid
            elif ptype in ("ByteProperty", "EnumProperty"):
                o += 8
            o += 1  # HasPropertyGuid
            voff = o
            if ptype == "IntProperty":
                val = struct.unpack_from("<i", b, o)[0]
            elif ptype == "ObjectProperty":
                val = struct.unpack_from("<i", b, o)[0]  # package index
            else:
                val = b[o:o + size].hex()
            props.append((pname, ptype, val, voff, size))
            o += size

    o = 0
    # UDataTable: its own UObject properties (RowStruct), then None, then rows.
    _, o = read_props(o)
    o += 4  # UObject "has GUID" bool for 4.17 exports
    rows = []
    nrows = struct.unpack_from("<i", b, o)[0]; o += 4
    for _ in range(nrows):
        rname, o = name_at(o)
        props, o = read_props(o)
        rows.append((rname, props))
    return rows, b


def main():
    uasset = sys.argv[1]
    uexp = uasset[:-len(".uasset")] + ".uexp"
    names = name_map(uasset)
    rows, data = parse(uexp, names)
    sets = {}
    if "--set" in sys.argv:
        for kv in sys.argv[sys.argv.index("--set") + 1:]:
            k, v = kv.split("=", 1)
            sets[k] = v
    out = bytearray(data)
    for rname, props in rows:
        shown = []
        for pname, ptype, val, voff, size in props:
            short = pname.split("_")[0]
            if short in sets:
                if ptype == "IntProperty":
                    struct.pack_into("<i", out, voff, int(sets[short]))
                elif ptype == "BoolProperty":
                    out[voff] = 1 if sets[short].lower() in ("1", "true") else 0
            shown.append(f"{short}={val}")
        print(f"{rname:12s} " + "  ".join(shown))
    if sets:
        open(uexp, "wb").write(out)
        print("patched:", uexp)


if __name__ == "__main__":
    main()
