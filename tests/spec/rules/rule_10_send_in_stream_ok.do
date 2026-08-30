// doot-spec: fmt
// expect-error: DT0046 at 4:1 "`stream` lands in v0.2, when SSE arrives"
// expect-error: DT0046 at 5:3 "`send` lands in v0.2 together with `stream` and SSE"
stream GET "/live" () {
  send <li>x</li>
}
