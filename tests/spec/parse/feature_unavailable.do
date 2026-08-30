// doot-spec: fmt
// expect-error: DT0046 at 4:3 "`spawn` lands in v0.2 together with tasks"
fn f() {
  spawn g()
}
