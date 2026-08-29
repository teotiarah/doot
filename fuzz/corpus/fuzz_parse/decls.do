pub fn greet(name: str = "x") -> str { return name }
pub type Alias = [str]
type Status enum { active, banned, pending }
@before(auth.require)
group "/admin" {
  route GET "/" () -> html! { return page() }
  route POST "/x" (form: NewUser) -> redirect! { return http.see_other("/") }
}
stream GET "/live" (room: str) {
  for m in topic.subscribe[Msg]("room:" + room) {
    send <li>${m.body}</li>
  }
}
test "it works" {
  test.eq(greet("a"), "a")
}
