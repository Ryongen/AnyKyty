#!/usr/bin/env python3
import argparse
import os
import random
import struct
import subprocess
import sys
import tempfile

MAGIC = 0x314352545954594B
PAGE = 4096
REGION = 4 << 20
EXT_BASE = 0x0800_0000_0000

OPS = {
    "PageManagerCreate": 1, "HasReadWatchers": 3, "IsWatched": 4, "WatchedSpan": 5, "AllowExecute": 6,
    "UpdatePageWatchers": 7, "HasHotPages": 8, "NoteWriteFault": 9, "CollectHotPageWrites": 10, "ForgetHotPages": 11,
    "TrackerCreate": 32, "IsRegionCpuModified": 34, "IsRegionGpuModified": 35, "MayBeGpuModified": 36,
    "MarkRegionAsCpuModified": 37, "UnmarkRegionAsGpuModified": 38, "InvalidateRegion": 39, "ForEachDownloadRange": 40,
    "RegionKeptSync": 41, "NoteGuestWriteFault": 42, "OpenWriteBlock": 43, "TakeKeptPages": 44, "RegionCpuDirtyEpoch": 45,
    "CpuModificationEpoch": 46, "DeferredProtectionPending": 47, "NoteHostWrite": 48, "RegionGpuCleanEpoch": 49,
    "ForEachCpuDirtySpan": 50, "CopyRegionCpuDirty": 51, "ConsumeCpuWrites": 52, "MarkRegionAsGpuModifiedUnlessCpuDirty": 53,
    "ForEachUploadRange": 54, "ForEachUploadRanges": 55, "CpuDirtyEpoch": 57, "GpuCleanEpoch": 58, "TakeOpenedUploadBytes": 59,
}
EV_DEFERRED_TAKEN = 8
EV_ARG_RANGE = 9


def check(words):
    value = 0x6A09E667F3BCC908
    for word in words[1:]:
        value = ((value ^ word) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return (value ^ (value >> 32) ^ (value >> 16)) & 0xFFFF


def record(op, seq, start, args, events=()):
    words = [0, 1, seq, 0, start, 0] + list(args)
    for kind, data in events:
        words.append(kind | (len(data) << 8))
        words.extend(data)
    total = len(words)
    words[0] = 0xA7 | (op << 8) | (len(args) << 16) | (total << 24)
    words[0] |= check(words) << 48
    return words


class Generator:
    def __init__(self, seed, count, hot):
        self.rng = random.Random(seed)
        self.count = count
        self.hot = hot
        self.bases = [0x2_0000_0000, 0x2_0000_0000 + REGION, 0x2_0000_0000 + 2 * REGION, EXT_BASE + 5 * REGION]
        self.watched = {}
        self.tracked = []
        self.sync = 1
        self.calls = []

    def address(self):
        rng = self.rng
        base = rng.choice(self.bases)
        page = rng.choice([rng.randrange(0, 1024), rng.randrange(0, 32), rng.randrange(1000, 1024), rng.randrange(0, 1024)])
        offset = rng.choice([0, 0, 0, rng.randrange(0, PAGE)])
        return base + page * PAGE + offset

    def size(self):
        rng = self.rng
        return rng.choice([1, PAGE, 2 * PAGE, rng.randrange(1, 16) * PAGE, rng.randrange(1, 65536), 64 * 1024, rng.randrange(1, 300) * PAGE,
                           REGION + rng.randrange(0, 8) * PAGE])

    def range(self):
        return self.address(), self.size()

    def pages(self, address, size):
        first = address // PAGE
        last = (address + size + PAGE - 1) // PAGE
        return range(first, last)

    def add(self, name, args, events=()):
        self.calls.append((OPS[name], list(args), list(events)))

    def watch(self):
        rng = self.rng
        if self.tracked and rng.random() < 0.45:
            address, size, is_read = self.tracked.pop(rng.randrange(len(self.tracked)))
            for page in self.pages(address, size):
                self.watched[(page, is_read)] -= 1
            self.add("UpdatePageWatchers", [address, size, 0, is_read])
            return
        address, size = self.range()
        size = min(size, 64 * PAGE)
        is_read = 1 if rng.random() < 0.3 else 0
        if any(self.watched.get((page, is_read), 0) >= 200 for page in self.pages(address, size)):
            return
        for page in self.pages(address, size):
            self.watched[(page, is_read)] = self.watched.get((page, is_read), 0) + 1
        self.tracked.append((address, size, is_read))
        self.add("UpdatePageWatchers", [address, size, 1, is_read])

    def step(self):
        rng = self.rng
        choice = rng.random()
        address, size = self.range()
        index = address // REGION
        if choice < 0.10:
            self.watch()
        elif choice < 0.16:
            self.add("OpenWriteBlock", [address, 64 * 1024, rng.random() < 0.6])
        elif choice < 0.22:
            self.add("InvalidateRegion", [address, size])
        elif choice < 0.26:
            self.add("UnmarkRegionAsGpuModified", [address, size])
            self.add("MarkRegionAsCpuModified", [address, size])
        elif choice < 0.30:
            self.add("ForEachDownloadRange", [address, size, rng.random() < 0.5])
        elif choice < 0.35:
            self.add("MarkRegionAsGpuModifiedUnlessCpuDirty", [address, size])
        elif choice < 0.43:
            is_written = 1 if rng.random() < 0.25 else 0
            sync = 0
            if not is_written and rng.random() < 0.5:
                self.sync += rng.choice([0, 1, 1, 2, 5])
                sync = self.sync
            defer = 1 if not is_written and rng.random() < 0.6 else 0
            events = [(EV_DEFERRED_TAKEN, [1, 0])] if defer and rng.random() < 0.5 else []
            self.add("ForEachUploadRange", [address, size, is_written, sync, defer], events)
        elif choice < 0.49:
            ranges = [self.range() for _ in range(rng.randrange(1, 6))]
            sync = 0
            if rng.random() < 0.5:
                self.sync += rng.choice([0, 1, 2])
                sync = self.sync
            defer = 1 if rng.random() < 0.6 else 0
            events = [(EV_ARG_RANGE, [a, s]) for a, s in ranges]
            if defer and rng.random() < 0.5:
                events.append((EV_DEFERRED_TAKEN, [1, 0]))
            self.add("ForEachUploadRanges", [len(ranges), sync, defer], events)
        elif choice < 0.53:
            self.add("NoteGuestWriteFault", [address])
        elif choice < 0.57:
            self.add("ConsumeCpuWrites", [rng.randrange(2), address, size])
        elif choice < 0.60:
            self.add("ForEachCpuDirtySpan", [address, size, rng.choice([0, PAGE, 64 * 1024])])
        elif choice < 0.63:
            self.add("CopyRegionCpuDirty", [index])
        elif choice < 0.68:
            self.add(rng.choice(["IsRegionCpuModified", "IsRegionGpuModified", "MayBeGpuModified", "CpuModificationEpoch", "NoteHostWrite"]),
                     [address, size])
        elif choice < 0.72:
            self.add(rng.choice(["RegionKeptSync", "RegionCpuDirtyEpoch", "RegionGpuCleanEpoch"]), [index])
        elif choice < 0.75:
            self.add(rng.choice(["TakeKeptPages", "CpuDirtyEpoch", "GpuCleanEpoch", "TakeOpenedUploadBytes", "HasHotPages"]), [])
        elif choice < 0.78:
            self.add("DeferredProtectionPending", [address])
        elif choice < 0.83:
            self.add(rng.choice(["IsWatched", "AllowExecute"]), [address])
        elif choice < 0.87:
            self.add(rng.choice(["HasReadWatchers", "WatchedSpan"]), [address, size])
        elif choice < 0.94:
            self.add("NoteWriteFault", [address])
        elif choice < 0.98:
            self.add("CollectHotPageWrites", [address, size] if rng.random() < 0.5 else [0, 0])
        else:
            self.add("ForgetHotPages", [address, size])

    def generate(self):
        self.add("PageManagerCreate", [0x1000, int(self.hot)])
        self.add("TrackerCreate", [0x2000, 0x1000, 1])
        while len(self.calls) < self.count:
            self.step()
        return self.calls


def write_trace(path, calls):
    words = [MAGIC, 1, 4096, REGION, os.getpid(), 1, 0, 0]
    start = 1_000_000_000
    rng = random.Random(len(calls))
    for seq, (op, args, events) in enumerate(calls, 1):
        start += rng.randrange(0, 400_000_000)
        words.extend(record(op, seq, start, args, events))
    with open(path, "wb") as out:
        out.write(struct.pack(f"<{len(words)}Q", *words))


def run(tools, path, hot):
    outs = []
    fatals = []
    for flavour in ("cpp", "rs"):
        out = f"{path}.{flavour}"
        command = [os.path.join(tools, f"tracker_replay_{flavour}"), path, "--out", out, "--digest-every", "1", "--apply-taken"]
        if hot:
            command.append("--hot")
        done = subprocess.run(command, capture_output=True, text=True)
        fatals.append([line for line in done.stderr.splitlines() if "fatal" in line])
        outs.append(out)
    compare = subprocess.run([os.path.join(tools, "tracker_compare"), outs[0], outs[1], "--show", "4"], capture_output=True, text=True)
    return compare.returncode == 0, compare.stdout, fatals


def shrink(tools, path, calls, hot):
    head, body = calls[:2], calls[2:]
    chunk = max(1, len(body) // 2)
    while chunk >= 1:
        index = 0
        changed = False
        while index < len(body):
            trial = body[:index] + body[index + chunk:]
            write_trace(path, head + trial)
            same, _, _ = run(tools, path, hot)
            if not same:
                body = trial
                changed = True
            else:
                index += chunk
        if not changed:
            chunk //= 2
    write_trace(path, head + body)
    return head + body


def main():
    parser = argparse.ArgumentParser(description="Random call sequences replayed on the C++ and the Rust memory tracker")
    parser.add_argument("--tools", required=True)
    parser.add_argument("--seeds", type=int, default=200)
    parser.add_argument("--first", type=int, default=1)
    parser.add_argument("--calls", type=int, default=3000)
    parser.add_argument("--hot", action="store_true")
    parser.add_argument("--keep", default="")
    args = parser.parse_args()
    work = args.keep or tempfile.mkdtemp(prefix="tracker-fuzz-")
    os.makedirs(work, exist_ok=True)
    differ = 0
    fatal_runs = 0
    for seed in range(args.first, args.first + args.seeds):
        calls = Generator(seed, args.calls, args.hot).generate()
        path = os.path.join(work, f"seed-{seed}.trace")
        write_trace(path, calls)
        same, report, fatals = run(args.tools, path, args.hot)
        if fatals[0] or fatals[1]:
            fatal_runs += 1
        if same:
            os.remove(path)
            for flavour in ("cpp", "rs"):
                os.remove(f"{path}.{flavour}")
            continue
        differ += 1
        print(f"seed {seed}: C++ and Rust differ\n{report}", flush=True)
        small = shrink(args.tools, path, calls, args.hot)
        _, report, fatals = run(args.tools, path, args.hot)
        print(f"seed {seed}: shrunk to {len(small)} calls ({path})\n{report}\nfatal C++: {fatals[0]}\nfatal Rust: {fatals[1]}", flush=True)
    print(f"{args.seeds} seeds of {args.calls} calls{' with hot pages' if args.hot else ''}: {differ} differ, {fatal_runs} ended on a fatal stop")
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main())
