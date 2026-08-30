// doot-spec: fmt
// expect-error: DT0065 at 4:20 "an attribute name must start with a letter or `_`, not byte 0x31"
// expect-error: DT0066 at 4:20 "an attribute value must be a quoted string or `${...}`"
let a = <div class=1></div>
