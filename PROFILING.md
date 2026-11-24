# A4091 profiling and coverage findings

## Test scope

The 2026-11-16 run used WinUAE's A4000D configuration with a 68040,
JIT disabled, and the GDB server. BindDrivers loaded the driver with both
`USE_PROFILING=1` and `USE_COVERAGE=1`. The driver was based on `c455116`
plus the uncommitted instrumentation changes, built with Amiga GCC
16.2.1b (16.2-rc23).

Devtest probed unit 0, checked geometry, and ran a read benchmark against
a disposable 64 MiB SCSI disk. No destructive write or integrity tests
were run. Physical A4091 and A4092 hardware was not tested.

The final extraction produced 137 call-graph arcs, zero dropped arcs, and
14 `.gcda` files. The interrupt counter was 233 at profile extraction.

These are call counts and coverage data, **not CPU-time measurements**.
There are no periodic time samples. Instrumentation changes execution
cost, particularly for small functions. Counts also include initialization
and diagnostic commands, not just the read benchmark. Profiling and
coverage were extracted sequentially, so their counts differ slightly.

## Findings and optimization candidates

### Expected timeout-list lifecycle

Coverage recorded 458 calls to `callout_remove()`:

- 229 calls removed the head of the timeout list.
- 229 calls found an already-unlinked entry, with neither link set.

This matches the intended request lifecycle: initialize a callout, arm it
with `callout_reset()`, then stop it on completion. Reset means schedule
or reschedule: removing before adding prevents duplicate list membership
when a timeout is already active. Using reset for the initial arm is
correct; its first removal is simply a harmless no-op.

The existing removal is O(1). These counts confirm expected API behavior,
not misuse or a demonstrated optimization opportunity. A separate initial
arming operation would introduce another caller precondition without a
measured benefit. See the callout functions in `port.c`.

### Frequent interrupt exclusion

The call profile recorded 688 `bsd_splbio()` / `bsd_splx()` pairs for
226 SCSI requests: roughly three pairs per request. These wrappers call
Exec's `Disable()` and `Enable()`.

Auditing redundant nesting or adjacent critical sections may reveal
opportunities to reduce overhead. However, the protections must not be
removed without checking task/interrupt concurrency and all callers.
Their high call frequency does not establish that they consume a large
fraction of CPU time. See the wrappers in `port.h` and their callers in
the SCSI request paths.

### Shared-interrupt rejection

The profile recorded 581 calls to `irq_handler_core()`, compared with
233 serviced interrupts. This is consistent with a shared interrupt
chain: entry into the handler does not imply that the A4091 needs service.

The early rejection path matters, but the handler already checks the
controller status and returns quickly when there is no relevant interrupt.
Quick interrupts were not exercised in this run. A controlled comparison
of normal and quick-interrupt modes would be more informative than
immediately rewriting the handler. See `attach.c`.

## Coverage gaps

The workload was narrower than aggregate line-coverage percentages alone
suggest:

- `callout_run_timeouts()` did not execute. Timeout expiration and recovery
  were not exercised; this does not by itself indicate a timer defect.
- Tagged-command paths did not execute.
- The Zorro-II bounce-buffer path did not execute.
- The timeout list never contained a second entry during the observed
  callout additions/removals.
- Quick-interrupt setup and handling were not exercised.

Consequently, this run provides limited evidence about deeper queues,
overlapping transfers, timeout recovery, and restricted DMA buffers.

READ_CAPACITY_16 returned unsupported-command sense `5/24/00`; other
capacity paths reported 131,072 sectors of 512 bytes. The emulator's
multi-GB/s benchmark results should not be used to estimate physical
A4091 throughput.

## Recommended next tests

1. Run a longer, controlled workload with small transfers and multiple
   outstanding requests.
2. Take before/after snapshots and compare deltas to separate workload
   activity from initialization and diagnostic commands.
3. Compare normal and quick-interrupt modes under the same workload.
4. Exercise timeout/recovery paths and Zorro-II bounce buffers explicitly.
5. Evaluate candidate changes using an uninstrumented build's throughput
   and latency, preferably on physical hardware as well as in WinUAE.

Broader workload coverage is the highest-value next step before larger
performance changes. No optimization speedup has yet been demonstrated.

## Instrumentation fixes discovered during testing

- Preserve all registers in `mcount()` and explicitly enable frame pointers.
- Exclude the profiling runtime from profiling and coverage instrumentation.
- Compile `a4091d` with the driver's `ENABLE_QUICKINTS` setting so both agree
  on the layout of `a4091_save_t`.

The HUNK linker omits local symbols from executables. The report generator
recovers their linked addresses from matching object files and the linker
map, then supplies them to gprof through `--external-symbol-table`.
Without those symbols, calls to static functions can be attributed to a
preceding exported symbol.

## Local artifacts

Use `tools/profile-report.mjs` to automate generation of the offline HTML
report from a new run. The README's "Offline Profiling and Coverage Report"
section describes the inputs and command line. The generator derives the
counts from each run and accepts optional environment/workload metadata;
it does not reuse the findings in this document as claims about new runs.

Generated artifacts from this run are under `build/winuae-profile/results/`:

- Interactive HTML report: `index.html`
- Full gprof report: `reports/gprof.txt`
- Coverage summary: `reports/coverage-summary.txt`
- Test details: `RESULTS.txt`

The same directory contains the raw `gmon.out`, `.gcda` files, command
output, annotated `.gcov` files, and matching `.gcno`/`.gcda` pairs under
`reports/objects/`. These are local build artifacts and may not be present
in another checkout.
