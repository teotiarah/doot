// doot-spec: fmt
// expect-error: DT0036 at 4:10 "`self` is only available inside a method body"
fn f() -> int {
  return self.x
}
