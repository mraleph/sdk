# Register Allocation

`AllocateRegisters` compiler pass is responsible for:

- assigning locations (CPU registers or spill slots) to values in the flow
graph according to the constraints specified by location summaries
(see `LocationSummary` class) attached to individual IL instructions;
- generating necessary moves (`ParallelMoveInstr`) between instructions to
ensure that values are placed in the locations that were assigned to them;
- generating stack maps for spill slots to ensure that GC sees values that
register allocator has spilled to the stack.

To give a concrete example imagine the following IL:

```dart
// Produce an output in some register
v0 <- Op(locs = {out: R})
// Call which produces a value in a fixed register
v1 <- Call(locs = {out: RAX, call})
// Consume values produced by v0 and v1 and produce
// output in RAX.
v2 <- Op(v0, v1, locs = {in: [R, R], out: [RAX]})
```

A valid result of register allocation for this code could look like this:

```dart
// Produce an output in RAX
v0 <- Op(locs = {out: RAX})
// Spill RAX into a stack slot across the call (all registers are caller-save).
ParallelMove S-1 <- RAX
v1 <- Call(locs = {out: RAX, call})
// Restore v0 into RBX and move v1 from RAX to RCX
// to free RAX as an output register for the next instruction.
ParallelMove RBX <- S-1, RCX <- RAX
v2 <- Op(v0, v1, locs = {in: [RBX, RCX], out: [RAX]})
```

## Linear Scan Register Allocation

Dart VM uses a variation of a linear scan register allocation algorithm, which
was originally described in [Linear scan register allocation][poletto-paper]
by Poletto and Sarkar. Our implementation is based on our experience working
with V8's Crankshaft register allocator, which in turn was based on the
implementation described in Christian Wimmer's Master Thesis
[Linear Scan Register Allocation for the Java HotSpot™ Client Compiler][wimmer-thesis].



[poletto-paper]: https://dl.acm.org/doi/10.1145/330249.330250
[wimmer-thesis]: http://www.christianwimmer.at/Publications/Wimmer04a/

