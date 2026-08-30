// doot-spec: fmt
// expect-error: DT0064 at 4:16 "a tag name must start with a letter or `_`, not byte 0x25"
// expect-error: DT0061 at 4:16 "closing `%` does not match the open tag"
let a = <div></%>
