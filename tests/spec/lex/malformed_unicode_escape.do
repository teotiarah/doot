// doot-spec: fmt
// expect-error: DT0015 at 3:10 "a unicode escape needs at least one hex digit and a closing `}`"
let a = "\u{zz}"
