# GC lock contention tooling

Tools for measuring how long threads wait for contended locks. They need no
VM changes or instrumented builds and cover all threads, including GC helper
threads. They report waiting time per call site and thread role (mutator TLAB
refill, write barrier, marking assist, scavenger task, concurrent marker, ...),
but not hold times.

* `futexctn/`: BPF-based tracer of futex waits, built from bcc's
  [futexctn](https://github.com/iovisor/bcc/blob/master/libbpf-tools/futexctn.c)
  with a small patch. No measurable overhead. Needs root or capabilities.
* `offcpu_from_perf.py`: blocked time per stack from perf context switch
  events. No root needed, but 20-50% overhead and large recordings.
* `run_dart_style.sh`, `dart_style.patch`: the dart_style formatter benchmark
  used for this work. The patch adds to `third_party/pkg/dart_style`:
  * a worker isolate pool (dart-lang/dart_style#1825);
  * `FORMAT_*` environment switches;
  * Perfetto timeline and profiler output.

Glibc mutexes block in the kernel with `FUTEX_WAIT` when contended, so both
tools see all contended `dart::Mutex` / `dart::Monitor` acquisitions which put
a thread to sleep.

Lock identity comes only from the call site and, for futexctn, the lock
address. The first frame after `pthread_mutex_lock` is usually missing because
libc has no frame pointers: the first VM frame shown is the caller of the
function that took the lock.

## Benchmark setup

```console
$ (cd third_party/pkg/dart_style && git apply ../../../runtime/tools/gc_contention/dart_style.patch)
$ tools/build.py -m release -a x64 gen_snapshot vm_platform.dill dartaotruntime
$ pkg/vm/tool/precompiler2 --packages=$PWD/.dart_tool/package_config.json \
    third_party/pkg/dart_style/bin/format.dart /tmp/format.aot
$ runtime/tools/gc_contention/run_dart_style.sh out/ReleaseX64/dartaotruntime \
    /tmp/format.aot 5 \
    --new_gen_semi_max_size=256 --new_gen_semi_initial_size=256 \
    --scavenger_tasks=8
```

The patch applies to dart_style at the revision pinned in `DEPS` (76978d23). The
configuration analyzed was 8 worker isolates in one group, with new space and
scavenger workers scaled to the number of isolates. Recompile `format.aot` with
a matching `gen_snapshot` when stub-relevant constants change (e.g.
`MarkingStackBlock::kSize`).

## futexctn

`futexctn` traces `FUTEX_WAIT` syscalls and aggregates the wait time per futex
address and user stack in the kernel. `futexctn.patch` adds these options:
* `-M`: aggregate over the threads of a process.
* `-a`: also trace `FUTEX_WAIT_BITSET`, i.e. condition variables
  (`Monitor::Wait`).
* `-r`: print raw output for offline symbolization, including inlined VM frames.
* `--max-entries`: size of the (lock, stack) map. Waits which do not fit are
  counted and reported (`FUTEXCTN_DROPPED` in raw output, a warning from
  `analyze_futexctn.py`).

```console
$ runtime/tools/gc_contention/futexctn/build_futexctn.sh /tmp/futexctn_build
$ runtime/tools/gc_contention/futexctn/run_futexctn.sh \
    /usr/local/bin/futexctn /tmp/futex_out -- \
    env FORMAT_POOL_SIZE=8 out/ReleaseX64/dartaotruntime <flags> \
    /tmp/format.aot -o none tests
$ python3 runtime/tools/gc_contention/futexctn/analyze_futexctn.py \
    --binary out/ReleaseX64/dartaotruntime /tmp/futex_out
```

The build needs clang with the BPF target, `libelf-dev` and `zlib1g-dev`.

`run_futexctn.sh` handles the process setup:
* starts the command stopped;
* attaches futexctn;
* resumes the command;
* saves its `/proc/<pid>/maps` for symbolization.

The report shows wait time by thread role, by call site and by lock address
(with its top call sites). Roles are matched by function names in the stack, so
e.g. sweeping done from a TLAB refill may be counted as "mutator: TLAB refill"
when the frame taking the lock is missing. Heap-allocated locks have no names;
identify them by their call sites. Their offset in `[heap]` (see `maps.txt`) is
stable across runs of the same binary.
With several output directories of the same binary, `analyze_futexctn.py`
averages them; `--json` saves the summary for comparing revisions.

futexctn needs root (`sudo run_futexctn.sh ...` runs the command as the invoking user) or a root-owned binary with these capabilities:

```console
$ sudo install -o root -g root -m 0755 \
    /tmp/futexctn_build/bcc/libbpf-tools/futexctn /usr/local/bin/futexctn
$ sudo setcap cap_bpf,cap_perfmon,cap_dac_read_search+ep /usr/local/bin/futexctn
```

Why each capability is needed:
* `CAP_BPF`: load the BPF programs.
* `CAP_PERFMON`: attach to tracepoints and collect stacks.
* `CAP_DAC_READ_SEARCH`: let libbpf read tracepoint IDs from tracefs, which is root-only.

Note that this lets every local user trace futex waits system-wide.

On the benchmark futexctn added no measurable overhead. Waits whose stack
collided with a different one in the BPF stack map are reported as
`[no stack]`; `run_futexctn.sh` uses a large stack map to keep them rare.

## Blocked time from perf context switches

Recording every context switch with its stack shows where threads block and for how long. No root is needed.

```console
$ FORMAT_POOL_SIZE=8 perf record -e context-switches -c 1 --call-graph fp \
    --switch-events -o /tmp/cs.data out/ReleaseX64/dartaotruntime \
    <flags> /tmp/format.aot -o none tests
$ perf script -i /tmp/cs.data -F tid,time,event,ip,sym,dso \
    --show-switch-events --show-mmap-events > /tmp/cs.txt
$ python3 runtime/tools/gc_contention/offcpu_from_perf.py \
    --binary out/ReleaseX64/dartaotruntime /tmp/cs.txt
```

Each context switch sample carries the stack at which the thread blocked. The time until the thread is switched back in is attributed to that stack. Waits in `pthread_mutex_lock` (`__lll_lock_wait`) are reported by thread role and call site.

Costs and requirements:
* About 20-50% wall time overhead.
* About 200 MB of recording per run of this benchmark.
* About a minute of analysis.
* `perf_event_paranoid <= 1`. Kernel stacks need -1.

## Measuring a stack of changes

To attribute contention to individual commits:
1. Build `dartaotruntime` for each revision.
2. Run `run_futexctn.sh` (or the perf recording) once per revision, plus `run_dart_style.sh` for wall time. Use a snapshot built by a matching `gen_snapshot`.
3. Compare the "by thread role", "by call site" and "by lock address" sections
   of adjacent revisions, averaged over a few runs each.
