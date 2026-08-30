// doot-spec: fmt
// expect-error: DT0065 at 3:14 "an attribute name must start with a letter or `_`, not byte 0x25"
let a = <div %></div>
