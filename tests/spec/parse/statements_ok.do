// doot-spec: fmt
// expect-ok
// expect-fmt-stable
fn f(xs: [int]) -> int {
  var total = 0
  for x in xs {
    if x > 0 {
      total += x
    } else if x == 0 {
      continue
    } else {
      break
    }
  }
  while total > 100 {
    total -= 1
  }
  defer log.info("done")
  return total
}
