// doot-spec: fmt
// expect-ok
// expect-fmt-stable
type T {
  x: int
}

fn T.get(self) -> int {
  return self.x
}
