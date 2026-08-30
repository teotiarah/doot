// doot-spec: fmt
// expect-error: DT0016 at 3:10 "U+110000 is not a scalar value"
let a = "\u{110000}"
