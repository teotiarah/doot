// doot-spec: fmt
// expect-error: DT0037 at 5:3 "`break` is only valid inside a loop"
// expect-error: DT0038 at 6:3 "`continue` is only valid inside a loop"
fn f() {
  break
  continue
}
