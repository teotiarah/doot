// doot-spec: fmt
// expect-error: DT0001 "`invalid_utf8.do` is not valid UTF-8; the first invalid byte is 0xff at offset 136"
let a = "ÿþ"
