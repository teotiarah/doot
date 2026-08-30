// doot-spec: fmt
// expect-error: DT0022 at 7:198 "nesting is deeper than 64 levels"
// expect-error: DT0030 at 7:199 "expected a tag name, found markup text"
// expect-error: DT0030 at 7:202 "expected a tag name, found markup text"
// expect-error: DT0030 at 7:458 "expected an expression, found /"
// expect-error: DT0032 at 7:459 "expected the statement to end, found identifier"
let a = <b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b><b>x</b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b></b>
