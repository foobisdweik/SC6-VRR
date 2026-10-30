#!/usr/bin/env python3
"""Print how much of each file macOS currently holds in RAM (the file cache).

It maps each file without reading it and asks the kernel with mincore() which
pages are resident. Usage: resident.py FILE...
"""
import ctypes, mmap, os, sys

libc = ctypes.CDLL(None, use_errno=True)
libc.mincore.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p]
page = os.sysconf("SC_PAGE_SIZE")
chunk = 1 << 30  # map 1 GiB at a time, so 15 GB files fit in the address space


def cached_bytes(f, off, length):
    # ctypes needs a writable buffer to take an address; a private mapping is
    # writable without changing the file, and mincore() never touches the pages.
    m = mmap.mmap(f.fileno(), length, flags=mmap.MAP_PRIVATE,
                  prot=mmap.PROT_READ | mmap.PROT_WRITE, offset=off)
    try:
        anchor = ctypes.c_char.from_buffer(m)
        vec = ctypes.create_string_buffer((length + page - 1) // page)
        rc = libc.mincore(ctypes.addressof(anchor), length, vec)
        del anchor  # release the buffer export so the mapping can close
        if rc != 0:
            raise OSError(ctypes.get_errno(), "mincore failed")
        return sum(b & 1 for b in vec.raw) * page
    finally:
        m.close()


total = cached = 0
for path in sys.argv[1:]:
    size = os.path.getsize(path)
    if size == 0:
        continue
    with open(path, "rb") as f:
        hit = sum(cached_bytes(f, off, min(chunk, size - off)) for off in range(0, size, chunk))
    hit = min(hit, size)
    total += size
    cached += hit
    print(f"{100 * hit / size:5.1f}%  {size / 1e9:6.2f} GB  {os.path.basename(path)}")
print(f"{100 * cached / max(total, 1):5.1f}%  {total / 1e9:6.2f} GB  total")
