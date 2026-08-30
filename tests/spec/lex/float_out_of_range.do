// doot-spec: fmt
// expect-error: DT0019 at 3:9 "this literal is outside the range of `float`, which is IEEE 754 double"
let a = 1.0e400
