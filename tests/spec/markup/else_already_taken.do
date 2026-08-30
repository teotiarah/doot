// doot-spec: fmt
// expect-error: DT0071 at 3:31 "a `{if}` chain has one `{else}`"
let a = <div>{if true}x{else}y{else}z{end}</div>
