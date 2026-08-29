fn page(u: User) -> html {
  return <div class="card" data-id="${u.id}">
    <h2>${u.name}</h2>
    {if u.bio != nil}
      <p>${u.bio}</p>
    {else if u.name != ""}
      <p>no bio</p>
    {end}
    <input name="q" value="${u.name}" required/>
    <br/>
    <!-- a comment -->
    <my-widget ...attrs/>
    <span ...${html.attrs(m)}/>
    <a href="/users/${u.id}">profile</a>
  </div>
}
