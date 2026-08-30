// doot-spec: fmt
// expect-error: DT0068 at 3:14 "a markup directive is `{if}`, `{else}`, `{for}`, or `{end}`"
let a = <div>{bogus}</div>
