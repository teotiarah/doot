// doot-spec: fmt
// expect-error: DT0046 at 53:1 "`stream` lands in v0.2, when SSE arrives"
// expect-error: DT0046 at 55:5 "`send` lands in v0.2 together with `stream` and SSE"
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
  let msgs = db.all[Msg](
    "select * from msgs where room = ? order by id desc limit 50",
    room,
  )!

  return layout(room, <div>
    <ul id="feed" data-live="/rooms/${room}/live">
      {for m in msgs}
        <li>${m.body}</li>
      {end}
    </ul>
    <form method="post" action="/rooms/${room}">
      <input name="body" required/>
      <button>send</button>
    </form>
  </div>)
}

type NewMsg {
  body: str @len(1, 500) @trim
}

route POST "/rooms/:room" (room: str, form: NewMsg) -> redirect! {
  let m = db.one[Msg](
    "insert into msgs (room, body, at) values (?, ?, ?) returning *",
    room,
    form.body,
    time.now(),
  )!

  topic.publish("room:" + room, m)
  return http.see_other("/rooms/" + room)
}

stream GET "/rooms/:room/live" (room: str) {
  for m in topic.subscribe[Msg]("room:" + room) {
    send <li>${m.body}</li>
  }
}
