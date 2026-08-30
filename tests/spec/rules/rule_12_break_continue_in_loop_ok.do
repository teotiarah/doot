// doot-spec: fmt
// expect-ok
// expect-fmt-stable
fn f(xs: [int]) {
  for x in xs {
    if x == 0 {
      continue
    }
    break
  }
}
