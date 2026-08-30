// doot-spec: fmt
// expect-error: DT0042 at 3:14 "`a` is declared twice"
fn f(a: int, a: int) {
}
