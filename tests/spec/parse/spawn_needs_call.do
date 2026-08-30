// doot-spec: fmt
// expect-error: DT0046 at 5:3 "`spawn` lands in v0.2 together with tasks"
// expect-error: DT0040 at 5:9 "`spawn` takes a function call"
fn f() {
  spawn a
}
