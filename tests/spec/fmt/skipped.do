// doot-spec: fmt
// expect-error: DT0046 at 6:3 "`spawn` lands in v0.2 together with tasks"
// expect-output:
// skipped 1 file with errors
fn f() {
  spawn g()
}
