# Parallel analysis benchmark

These tools measure how much faster `ParallelAnalysis` (`lib/src/dart/analysis/parallel`) analyzes a directory with several worker isolates than with one. They work on Linux and macOS.

- [`parallel_analysis.dart`](parallel_analysis.dart): the benchmark. It analyzes a directory once and prints the time, the peak RSS, and optionally a dump of all diagnostics, or a timeline.
- [`parallel_analysis_run.dart`](parallel_analysis_run.dart): the driver. It runs the AOT-compiled benchmark with several worker counts, cold and hot, checks that the diagnostics are identical to the first run, and prints median times and speedups.
- [`parallel_analysis_flutter.sh`](parallel_analysis_flutter.sh): prepares a copy of a Flutter checkout, so that its packages can be analyzed.

A run with 1 worker uses the usual sequential analysis. With more workers, the worker isolates are in the same isolate group and share the analyzer state. This relies on persistent handles, which is not a supported way to share objects between isolates.

**Cold** runs delete the cache directory first. **Hot** runs reuse the cache of the previous cold run.

## Build

Build the runtime, `gen_snapshot`, the platform, and the SDK from the SDK checkout. The build output goes to `xcodebuild/<config>` on macOS and to `out/<config>` on Linux. The analyzer finds the SDK libraries in `dart-sdk`, next to the runtime.

On Apple Silicon:

```
tools/build.py -m release -a arm64 create_sdk dartaotruntime gen_snapshot vm_platform.dill
```

On Linux x64:

```
tools/build.py -m release -a x64 create_sdk dartaotruntime gen_snapshot vm_platform.dill
```

The driver compiles the benchmark into an AOT snapshot with `pkg/vm/tool/precompiler2` when the snapshot does not exist. Pass `--rebuild` after changing the analyzer. The driver uses `xcodebuild/ReleaseARM64` on Apple Silicon, and `out/ReleaseX64` on Linux x64.

## Run on `pkg/analyzer`

The driver needs only `dart:` libraries, so the checked-in SDK can run it:

```
tools/sdks/dart-sdk/bin/dart pkg/analyzer/tool/benchmark/parallel_analysis_run.dart \
  --root pkg/analyzer --workers 1,2,4,8 --runs 3
```

Do not change the analyzed directory while the benchmark runs: the first run produces the reference diagnostics, and every other run must match them. Results go to `$TMPDIR/parallel_analysis` (`--work`). A run with 3 runs of 4 worker counts takes about 5 minutes on a fast Linux machine.

Example output:

```
## /path/to/sdk/pkg/analyzer, cold

| workers | wall (ms) | speedup | CPU (s) | peak RSS (MB) | same |
|---:|---:|---:|---:|---:|:---:|
| 1 | 15932 | 1.00x | 19.0 | 1296 | yes |
| 8 | 4329 | 3.68x | 26.1 | 1051 | yes |
```

- `same`: whether the diagnostics of all runs are byte-identical to the first run.
- `CPU`: user plus system time, from `/usr/bin/time`.

## Run on Flutter packages

Run `bin/flutter --version` in the Flutter checkout once, so that `bin/cache` has the Dart SDK and the engine version. Then prepare a copy of the checkout:

```
pkg/analyzer/tool/benchmark/parallel_analysis_flutter.sh \
  $HOME/src/flutter/flutter $TMPDIR/parallel_analysis/flutter
```

The script copies the checkout without `.git` and `bin/cache`, and downloads `sky_engine` for the engine version, because it provides `dart:ui`. It then resolves the workspace with `pub get`, which needs network access and adds packages to `~/.pub-cache`. The checkout itself is not changed.

Then run the driver:

```
tools/sdks/dart-sdk/bin/dart pkg/analyzer/tool/benchmark/parallel_analysis_run.dart \
  --root $TMPDIR/parallel_analysis/flutter/packages --workers 1,2,4,8 --runs 3
```

`packages` has two analysis contexts, `packages` and `packages/flutter_tools`. They are analyzed one after another, each with its own workers. Most of the 63K diagnostics are errors in the generated `animated_icons/data` parts. They are the same with any number of workers.

## Reference results

From a 96-core Linux x64 machine, 3 runs, medians:

| corpus | 1 worker | 2 | 4 | 8 | 16 |
|---|---:|---:|---:|---:|---:|
| `pkg/analyzer`, cold | 15.9 s | 1.67x | 2.58x | 3.68x | 4.05x |
| `pkg/analyzer`, hot | 1.54 s | 1.32x | 1.50x | 1.69x | 1.76x |
| Flutter `packages`, cold | 54.2 s | 1.78x | 2.88x | 4.69x | 5.22x |
| Flutter `packages`, hot | 3.20 s | 1.34x | 1.50x | 1.69x | 1.72x |

On a MacBook, use at most as many workers as there are performance cores. Keep the machine on power and cool, because thermal throttling makes runs with many workers slower than they should be.

## More options

- `--vm-flags "<flags>"` passes VM flags to every run. For example, `--old_gen_growth_space_ratio=80` gives the old generation more headroom, so there are fewer marking cycles. That makes 8 workers about 10% faster, but uses more memory. `--verbose_gc` prints every GC.
- `--modes cold` or `--modes hot` runs only one of them.
- The benchmark itself can be run directly, e.g. to record a timeline of all tasks and GC events in the Chrome trace format, viewable in Perfetto:

  ```
  xcodebuild/ReleaseARM64/dartaotruntime $TMPDIR/parallel_analysis/parallel_analysis.aot \
    --root pkg/analyzer --cache /tmp/pa-cache --cold --workers 8 --timeline /tmp/pa.json
  ```

  Other options: `--dump <file>` writes sorted diagnostics, `--workers-statistics` prints tasks per worker, `--no-retain-asts` parses files again for linking, and `--parallel-parse-cycle-size=0` disables parsing large library cycles in parallel tasks.
