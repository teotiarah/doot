// doot-spec: fmt
// expect-error: DT0021 at 4:11 "this interpolation is never closed"
// expect-error: DT0031 at 4:15 "expected `}` to close the interpolation, but the file ends here"
let a = "x${y
