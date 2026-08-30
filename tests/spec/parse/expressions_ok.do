// doot-spec: fmt
// expect-ok
// expect-fmt-stable
fn f(xs: [int], m: {str: int}) -> int {
  let a = 1 + 2 * 3
  let b = (1 + 2) * 3
  let c = not a == b
  let d = a > 1 and b < 2 or not c
  let e = xs.map(fn(x: int) => x + 1)
  let g = m["k"] else 0
  let h = xs as [int]
  return a
}
