// doot-spec: fmt
// expect-ok
// expect-fmt-stable
type User {
  id:   int
  name: str
  bio:  str?
}

type Status enum { active, banned }

type Id = int

fn User.display(self) -> str {
  return self.name
}

pub fn greet(name: str) -> str {
  return "hello ${name}"
}

let limit = 50

test "it greets" {
  let g = greet("x")
}
