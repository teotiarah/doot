// doot-spec: fmt
// expect-ok
// expect-fmt-stable
fn page(title: str, xs: [str], flag: bool) -> html {
  return <html>
    <head><title>${title}</title></head>
    <body>
      <!-- a comment -->
      <img src="/a.png" alt=""/>
      <br/>
      <ul>
        {for x in xs}
          <li>${x}</li>
        {else}
          <li>none</li>
        {end}
      </ul>
      {if flag}
        <p>on</p>
      {else}
        <p>off</p>
      {end}
    </body>
  </html>
}
