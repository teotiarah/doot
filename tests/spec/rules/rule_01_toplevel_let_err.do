// doot-spec: fmt
// expect-error: DT0033 at 3:1 "a top-level binding must be `let`; state belongs in SQLite, in request scope, or in a `cache` cell"
var a = 1
