fn f(a: int, b: [str], c: {str: int}, d: fn(int) -> str) -> int! {
  var total = 0
  for i, v in b {
    if i > 0 and total < 10 {
      total += 1
    } else if i == 0 {
      continue
    } else {
      break
    }
  }
  match a {
    .active -> render()
    1 | 2 -> two()
    else -> {
      log.warn("other")
    }
  }
  while total > 0 {
    total -= 1
  }
  defer cleanup()
  return total
}
