route GET "/rooms/:room" (room: str) -> html! {
  let msgs = db.all[Msg](`select * from msgs where room = ?`, room)!
  return layout(room, body)
}
