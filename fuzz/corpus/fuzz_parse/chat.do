type Msg {
  id:   int
  room: str
  body: str
  at:   time.Time
}

fn layout(title: str, body: html) -> html {
  return <html>
    <head><title>${title}</title></head>
    <body>${body}</body>
  </html>
}

route GET "/rooms/:room" (room: str) -> html! {
  let msgs = db.all[Msg]("select * from msgs where room = ?", room)!
  return layout(room, <ul id="feed" data-live="/rooms/${room}/live">
    {for m in msgs}
      <li>${m.body}</li>
    {else}
      <li class="empty">nobody here</li>
    {end}
  </ul>)
}
