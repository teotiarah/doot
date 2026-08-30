// doot-spec: fmt
// expect-error: DT0061 at 4:23 "closing `div` does not match the open tag"
// expect-error: DT0060 at 4:9 "this element is never closed"
let a = <div><span>x</div>
