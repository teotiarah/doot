// doot-spec: fmt
// expect-error: DT0035 at 5:3 "`send` is only valid inside a `stream` body"
// expect-error: DT0046 at 5:3 "`send` lands in v0.2 together with `stream` and SSE"
fn f() {
  send <li>x</li>
}
