// doot-spec: fmt
// expect-error: DT0063 at 5:12 "`br` is a void element and takes no closing tag"
// expect-error: DT0030 at 5:18 "expected an expression, found /"
// expect-error: DT0032 at 5:19 "expected the statement to end, found identifier"
let a = <p></br></p>
