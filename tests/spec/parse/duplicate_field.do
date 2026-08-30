// doot-spec: fmt
// expect-error: DT0043 at 5:3 "`x` is declared twice"
type T {
  x: int
  x: str
}
