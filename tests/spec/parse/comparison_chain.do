// doot-spec: fmt
// expect-error: DT0039 at 3:15 "comparison operators do not chain; write `a < b and b < c`"
let a = 1 < 2 < 3
