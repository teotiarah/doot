// doot-spec: fmt
// expect-error: DT0034 at 7:1 "a method's first parameter must be `self`"
type T {
  x: int
}

fn T.get(other: int) -> int {
  return other
}
