# Component boundaries

doot is two programs. This document draws the line between them, and it is written now — long before the second one is built — because the questions it settles are exactly the ones that become expensive to answer later.

| | **the runtime** (`doot`) | **the panel** (`doot-panel`) |
| --- | --- | --- |
| Ships | v0.1 | v0.6 |
| Runs | one application | one machine, several applications |
| Speaks | plain HTTP/1.1 | HTTPS to the internet, plain HTTP to apps |
| Knows about | its own app | processes, sockets, certificates, deploys |
| Required? | yes | no — an app runs perfectly well alone |

---

## The rule

> **The runtime never listens on TLS. It speaks plain HTTP/1.1 on a TCP port or a Unix domain socket. Certificates, ACME, public ports, and TLS termination belong entirely to the panel.**

[D011](01-decisions.md#d011). This is a permanent property of the runtime, not a v0.1 limitation.

## Why this is settled now

Ruling out nginx and Caddy from the deployment path initially appears to force TLS into the app runtime. It does not — it puts TLS in the **panel**, which is the component that already terminates HTTP in order to do host-header routing, and which therefore already owns the public network edge.

If both components could terminate TLS, every deployment would raise questions with no good answer:

- Which one holds the certificate?
- Which one owns port 443?
- What happens when both attempt renewal?
- If the app also listens publicly, is the panel's routing being bypassed?
- When a request arrives over TLS at the app directly, does `req.scheme` mean what the app thinks it means?

Each is individually solvable and collectively a swamp. Answering them once, in advance, in favor of the panel, costs only standalone HTTPS — and standalone HTTPS is precisely what the panel exists to provide.

---

## Ownership

### The runtime owns

- The language, compiler, and VM
- Its own event loop, tasks, and scheduling
- Its SQLite database file
- Request handling, routing, rendering, sessions, and application logic
- Its structured log stream, written to stdout
- Per-request memory and CPU budgets ([D005](01-decisions.md#d005))
- Its listening socket — **whose address it is told, and does not choose**

### The panel owns

- The public network edge: ports 80 and 443
- TLS termination and ACME certificate issuance and renewal
- Host-header routing to app processes
- Process supervision: start, stop, restart, health check, crash recovery
- **Unix socket path assignment** — one per app, and therefore no port allocation
- cgroups resource limits per app: memory ceiling, CPU weight, task count
- Git-based deploys: fetch, compile with `doot build`, swap, roll back
- Log collection from app stdout, retention, and search
- Aggregate metrics across apps

### Neither owns

- Containers, images, or a registry
- An orchestrator, a scheduler, or a service mesh
- A reverse proxy as a separate program
- Anything that spans more than one machine ([00-vision.md](00-vision.md#non-goals-permanently))

---

## The interface

The two components meet at exactly three places. Keeping the interface this narrow is what allows either side to change independently.

### 1. The listening socket

In production the runtime binds a Unix domain socket at a path the panel assigns:

```
doot run --listen unix:/run/doot/myapp.sock
```

Unix sockets rather than localhost ports, deliberately:

- **No port allocation**, therefore no conflicts between co-hosted apps and no registry of assignments to keep consistent
- **No accidental public exposure** — a filesystem socket cannot be reached from off-box, so a misconfigured firewall cannot expose an app process
- **Filesystem permissions** are the access control, which is one mechanism instead of two
- Slightly lower latency, which is incidental but free

In development the runtime binds a TCP port, because a browser needs to reach it directly:

```
doot dev            # listens on 127.0.0.1:8080
```

### 2. Forwarded request metadata

The panel adds three headers, and the runtime trusts them **only** when started in panel mode with a Unix socket listener — never on a TCP listener, so a forged header from a direct connection cannot lie about the client:

```
X-Doot-Client-IP     the real client address
X-Doot-Scheme        http | https  — what the client used, for absolute URL construction
X-Doot-Host          the original Host header
```

Three headers with a `X-Doot-` prefix rather than the `X-Forwarded-*` conventions, because those are ambiguous by history (comma-joined chains, inconsistent trust semantics) and there is no third-party proxy here to interoperate with.

### 3. Process lifecycle

- The panel starts the runtime with `--listen` and an environment
- The runtime writes structured JSON logs to stdout; the panel collects them
- `SIGTERM` begins graceful shutdown: stop accepting, finish in-flight requests, close SSE streams so browsers reconnect to the replacement, flush, exit
- `GET /_doot/health` returns readiness; the panel polls it before routing traffic to a new process
- A non-zero exit is a crash, and the panel restarts with backoff

That is the entire contract. The runtime does not know the panel exists, and an app started by hand behaves identically except that nobody is terminating TLS in front of it.

---

## Deployment shapes

**Development.** `doot dev`, TCP on localhost, hot reload, no panel.

**Single app, no panel.** `doot build`, copy the binary, run it behind whatever you already have, or on a port with no TLS. Fully supported — the panel is a convenience, not a dependency.

**The intended production shape.** The panel on a VPS, several apps, each on a Unix socket, each in its own cgroup, TLS terminated once at the edge, deploys from git.

**What is never a shape.** Multiple machines. Load balancing across hosts. A shared database between apps. Containers. If an application outgrows one box, it has outgrown doot, and that is a stated property rather than a defect ([00-vision.md](00-vision.md#non-goals-permanently)).

---

## The panel is single-user, by design

A personal tool for running several of your own applications on your own machine. It is **not** a hosting platform, not multi-tenant, and has no concept of organizations, teams, quotas, or billing.

This matters because it is what makes the isolation story honest. cgroups plus Unix socket permissions plus separate database files is entirely adequate protection between **your own** applications — where the threat is one app misbehaving and starving the others, not one tenant attacking another. The same design would be inadequate for untrusted tenants, and the answer to that is that doot does not serve untrusted tenants, rather than a hardening roadmap.

Being explicit about the threat model now is what keeps the panel from accreting the container runtime that a multi-tenant version would genuinely require.
