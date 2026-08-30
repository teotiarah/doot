// doot-spec: fmt
// expect-error: DT0041 at 4:9 "`defer` takes a function call"
fn f() {
  defer a
}
