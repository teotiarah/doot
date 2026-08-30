// doot-spec: fmt
// expect-error: DT0022 at 6:137 "expressions nest deeper than 128 levels"
// expect-error: DT0030 at 6:137 "expected `)` to close the group, found ("
// expect-error: DT0030 at 6:138 "expected `)` to close the arguments, found ("
// expect-error: DT0030 at 6:139 "expected `)` to close the arguments, found integer literal"
let a = ((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((1))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))
