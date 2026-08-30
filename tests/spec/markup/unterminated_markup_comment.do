// doot-spec: fmt
// expect-error: DT0072 at 4:14 "this markup comment is never closed"
// expect-error: DT0060 at 4:9 "this element is never closed"
let a = <div><!-- never closed
