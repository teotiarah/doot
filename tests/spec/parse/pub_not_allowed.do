// doot-spec: fmt
// expect-error: DT0047 at 3:1 "`pub` marks a function, type, or binding as exported; a route is reachable by its path already"
pub route GET "/" () -> html {
  return <p>hi</p>
}
