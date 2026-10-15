"""Summarize macOS `sample` reports of SoulCalibur VI running under CrossOver.

Usage: python3 -I analyze_sample.py <sample-*.txt>... [--top N]

For each thread it reports how many snapshots found the thread running versus
waiting (blocked in a wait or receive call at the top of its stack). For the
busiest threads it lists the hottest code locations:
  - game exe addresses (0x140000000-0x150000000), to map to Ghidra functions,
  - named frames (DXMT, Metal, Wine, Rosetta).
"Self" counts the deepest frame of each running snapshot. Under Rosetta the
deepest frame is often a Rosetta JIT or runtime frame; in that case the deepest
game-exe frame in the same stack is counted as "game self" instead.
"""
import collections
import re
import sys

LINE = re.compile(r"^(?P<prefix>[ +!:|]*)(?P<count>\d+) (?P<rest>.*)$")
WAIT = re.compile(
    r"mach_msg|__psynch_cvwait|__psynch_mutexwait|__semwait_signal|__ulock_wait|semaphore_wait|"
    r"__workq_kernreturn|kevent|__select|__wait4|__sigsuspend|_pthread_cond_wait|nanosleep|"
    r"__recvfrom|__read_nocancel|poll|usleep|thread_suspend")
ADDR = re.compile(r"\[(0x[0-9a-f]+)\]")
GAME_LO, GAME_HI = 0x140000000, 0x150000000


def parse(path):
    """Yield (thread_name, total, [(depth, count, text)]) per thread."""
    threads = []
    in_graph = False
    cur = None
    for raw in open(path, errors="replace"):
        line = raw.rstrip("\n")
        if line.startswith("Call graph:"):
            in_graph = True
            continue
        if not in_graph:
            continue
        if line.startswith("Total number in stack") or line.startswith("Sort by top of stack"):
            break
        m = LINE.match(line)
        if not m:
            continue
        depth = len(m.group("prefix"))
        count = int(m.group("count"))
        text = m.group("rest")
        if depth == 4 and text.startswith("Thread_"):
            cur = [text, count, []]
            threads.append(cur)
        elif cur is not None:
            cur[2].append((depth, count, text))
    return threads


def addr_of(text):
    m = ADDR.search(text)
    return int(m.group(1), 16) if m else None


SYSLIB_LO = 0x7ff800000000  # translated x86 system libraries (unnamed under Rosetta)


def shared_wait_leaves(paths):
    """Unnamed system-library leaf addresses shared by 3 or more threads.

    Under Rosetta the wait primitives (mach_msg, psynch, ulock) have no names.
    Many idle threads sit in the same few of them, so a system-library leaf
    address that 3 or more threads share is treated as a wait."""
    seen = collections.defaultdict(set)
    for path in paths:
        for name, total, nodes in parse(path):
            for i, (depth, count, text) in enumerate(nodes):
                nxt = nodes[i + 1][0] if i + 1 < len(nodes) else -1
                a = addr_of(text)
                if nxt <= depth and a and a >= SYSLIB_LO and "(in <unknown binary>)" in text:
                    seen[a].add(name)
    return {a for a, threads in seen.items() if len(threads) >= 3}


def analyze(paths, top):
    waits = shared_wait_leaves(paths)
    per_thread = collections.defaultdict(lambda: [0, 0])  # name -> [total, running]
    hot = collections.defaultdict(collections.Counter)    # name -> Counter(location)
    for path in paths:
        for name, total, nodes in parse(path):
            short = re.sub(r"Thread_\d+:?\s*", "", name).strip() or name.split()[0]
            key = short if short != name else name.split()[0]
            per_thread[key][0] += total
            # Walk the tree; a node is a leaf when the next node is not deeper.
            stack = []
            for i, (depth, count, text) in enumerate(nodes):
                while stack and stack[-1][0] >= depth:
                    stack.pop()
                stack.append((depth, count, text))
                nxt = nodes[i + 1][0] if i + 1 < len(nodes) else -1
                if nxt > depth:
                    continue
                # Leaf: `count` snapshots ended here. Children may account for
                # part of a parent's count; leaves are exact.
                if WAIT.search(text) or addr_of(text) in waits:
                    continue
                per_thread[key][1] += count
                game = [t for _, _, t in stack if (a := addr_of(t)) and GAME_LO <= a < GAME_HI]
                if game:
                    hot[key]["game " + hex(addr_of(game[-1]))] += count
                else:
                    hot[key][re.sub(r"\s+\[0x[0-9a-f]+\]$", "", text).strip()[:90]] += count
    rows = sorted(per_thread.items(), key=lambda kv: -kv[1][1])
    print(f"{'thread':48s} {'snapshots':>9s} {'running':>8s} {'busy%':>6s}")
    for name, (total, running) in rows[:top]:
        print(f"{name[:48]:48s} {total:9d} {running:8d} {100.0 * running / max(total, 1):5.1f}%")
    for name, _ in rows[:4]:
        print(f"\nhottest in {name}:")
        for loc, n in hot[name].most_common(12):
            print(f"  {n:6d}  {loc}")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    top = 15
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1])
        args.remove(str(top))
    analyze(args, top)
