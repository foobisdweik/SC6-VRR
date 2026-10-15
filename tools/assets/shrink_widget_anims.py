"""Shorten the fade animations in SC6's boot "Advertise" widget (UE 4.17, cooked).

Usage: python3 -I shrink_widget_anims.py <Advertise.uasset> [new_seconds]

The widget's fades (anm_fadein, anm_fadeout, and a 0.1 s fade) are color
tracks. Their length is stored in four places, which must stay consistent:
  1. RichCurveKey times (27-byte native keys; the time is at byte +3),
  2. FloatProperty "EndTime" (section ends),
  3. FloatProperty "Value" inside FloatRangeBound structs (playback ranges),
  4. natively serialized TRange<float> bounds in the compiled evaluation
     template (bound type byte 1 = Inclusive, then the float).
Every 0.5 s and 0.1 s value in those places becomes new_seconds (default 1/60 s,
one frame). Zero is avoided: the boot script waits on the animation end time
with a timer, and a zero-length timer may never fire. Values are rewritten in
place, so the package layout does not change. Writes <name>.uexp next to the input.
"""
import importlib.util
import os
import struct
import sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("dt", os.path.join(here, "dt_dump.py"))
dt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dt)

OLD_TIMES = (0.5, 0.1)


def main():
    uasset = sys.argv[1]
    new = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0 / 60.0
    uexp = uasset[:-len(".uasset")] + ".uexp"
    names = dt.name_map(uasset)
    b = bytearray(open(uexp, "rb").read())

    def name_at(o):
        i, n = struct.unpack_from("<ii", b, o)
        return names[i] if 0 <= i < len(names) and n == 0 else None

    targets = {}  # offset -> kind

    # 1. Curve keys. Array inner tag: Name Type Size Index StructName Guid HasGuid.
    rk = struct.pack("<ii", names.index("RichCurveKey"), 0)
    o = 0
    while (o := b.find(rk, o)) >= 0:
        count = struct.unpack_from("<i", b, o - 28)[0]
        first = o + 8 + 16 + 1
        for k in range(count):
            targets[first + k * 27 + 3] = "key"
        o += 8

    for old in OLD_TIMES:
        ob = struct.pack("<f", old)
        o = 0
        while (o := b.find(ob, o)) >= 0:
            if o in targets:
                pass
            # 2 and 3. Tagged float: Name(8) Type(8) Size(4) Index(4) HasGuid(1) Value.
            elif name_at(o - 17) == "FloatProperty" and name_at(o - 25) in ("EndTime", "Value"):
                targets[o] = name_at(o - 25)
            # 4. Native TRange<float>: [type][lower float][type][upper float], both Inclusive.
            elif b[o - 1] == 1 and b[o - 6] == 1:
                targets[o] = "range"
            o += 1

    counts = {}
    for off, kind in sorted(targets.items()):
        old = struct.unpack_from("<f", b, off)[0]
        if not any(abs(old - t) < 1e-6 for t in OLD_TIMES):
            continue  # keys at time 0 stay at 0
        struct.pack_into("<f", b, off, new)
        counts[(kind, round(old, 4))] = counts.get((kind, round(old, 4)), 0) + 1
    for (kind, old), n in sorted(counts.items()):
        print(f"{n:3d}  {kind:8s} {old} -> {new:.6f}")
    open(uexp, "wb").write(b)
    print("patched:", uexp)


if __name__ == "__main__":
    main()
