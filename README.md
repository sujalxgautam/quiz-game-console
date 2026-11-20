# Quiz Game — Console Edition (C++17)

A terminal quiz game that fetches live trivia questions from
[The Trivia API](https://the-trivia-api.com) over raw POSIX sockets and
falls back to a built-in local question bank when the network is
unavailable or the request would exceed the free tier's usefulness.

A deliberately dependency-free C++17 project — standard library only,
plus raw POSIX sockets for HTTP — built to teach HTTP from first
principles, JSON scraping by hand, and **honest degradation** when a
free API can't back the promise you made.

## Difficulty

**Medium.** This is the first project in the set that talks to the
network. It requires more care than the games: connection handling,
timeouts, partial reads, and a real design decision about when *not* to
call the API.

## Features

- **Live questions** from The Trivia API for small quizzes (≤ 25 questions).
- **Local fallback bank** (23 questions) for offline play and for large quizzes.
- **Category and difficulty menus** (the API supports both, though the
  local bank only uses its own tags).
- **Question count picker:** 10, 25, 50, or 100.
- **Score with a results screen** and a percentage bar.
- **Hand-written HTTP/1.1 client** — no libcurl, no dependencies.

## The tiered-fetch design decision

The Trivia API's **free tier has no session tokens**. Every call is an
independent random roll — ask for 50 questions now, 50 again a second
later, and the same questions will come back. Asking for 100 in two
batches makes it worse (rate limits + duplicates).

Rather than patch around this (client-side dedup, retries, longer
sleeps), the honest fix is to **not make the promise in the first
place**:

- `count ≤ 25` → fetch from the API, deduplicate by question text,
  top up from the local bank if short. This is where "live" actually
  delivers value.
- `count > 25` → skip the API entirely. Use the local bank, shuffled.

The menu **labels which tier each option uses**, so the player knows
what they're getting. The trade-off (losing live questions for big
quizzes) is stated in the README rather than hidden. A future polish
pass with a paid API key or a switch to Open Trivia DB (which does have
free session tokens) would restore live fetching at all sizes — and the
change would be contained to `triviaApi::fetch()` and the `policy`
constant, because the rest of the code doesn't know or care where the
questions come from.

## Controls

| Key | Action |
|-----|--------|
| `1`–`6` | Category menu |
| `1`–`4` | Difficulty menu / count menu |
| `A`/`B`/`C`/`D` | Answer a question |
| `Enter` | Next question (after answering) |
| `R` / `M` / `Q` | Play again / Main menu / Quit (results screen) |
| `Q` | Quit (any menu) |

## Build & Run

### Quick build (g++)

```bash
g++ -std=c++17 -O2 -pthread main.cpp -o quiz
./quiz
```

### With CMake

```bash
cmake -B build
cmake --build build
./quiz
```

## How it works

- **Raw HTTP client.** `http::get(host, path)` opens a TCP socket with
  `getaddrinfo` + `socket` + `connect`, sends a hand-built HTTP/1.1 GET
  request, and reads the response **incrementally** with `recv`. It stops
  as soon as the response body contains a complete top-level JSON array —
  it does not wait for the server to close the connection, because
  HTTP/1.1 servers usually don't.
- **Chunked transfer decoding.** The response headers are checked for
  `Transfer-Encoding: chunked`; if present, the body is decoded before
  parsing. This is what lets the code work against real-world HTTP/1.1
  servers that use chunked encoding by default.
- **JSON by hand.** `json::stringValue`, `json::questionText`, and
  `json::stringArray` are small string-search helpers. They are not a
  general JSON parser — they exist to extract the five fields the
  question objects contain. This is a teaching trade-off: adding a real
  JSON library would add a dependency and hide the mechanics.
- **Tiered fetch.** `quiz::buildQuestionSet(count, rng)` decides whether
  to call the API based on `policy::kApiMaxCount`, tops up from the local
  bank if needed, and shuffles the combined set so local-bank questions
  aren't clustered at the end.
- **10-second timeout.** Both `SO_RCVTIMEO` and `SO_SNDTIMEO` are set to
  10 s on the socket, so a stalled connection fails fast and the local
  bank kicks in. This is why the game is playable offline — the only
  cost is a 10-second wait on the first fetch attempt.

## What this project teaches

- **HTTP from first principles** — sockets, request format, response
  format, `Connection: close`, chunked encoding, timeouts.
- **Reading from a socket is not like reading from a file** — `recv`
  returns data in chunks, and "the response is complete" is a property
  of the *content*, not of the connection closing.
- **JSON parsing by hand** — the mechanics of walking a string and
  extracting fields. It makes the case for a real library visible.
- **Honest degradation** — what to do when a free API can't back the
  promise you made. The lesson is *change the contract*, not patch
  around it.
- **POSIX sockets** — the "everything is a file descriptor" model,
  `getaddrinfo`, error handling for connection failure.
- **Threads for timing** — `sleep_for` between fetch retries.

## Requirements

- **Compiler:** any C++17 compiler (`g++`, `clang++`)
- **Platform:** **Linux or macOS only.** Uses raw POSIX sockets
  (`<arpa/inet.h>`, `<sys/socket.h>`, `<unistd.h>`). Will not compile on
  Windows with MSVC.
- **Network:** an internet connection for live questions. The game is
  fully playable offline via the local bank.
- **Dependencies:** standard library only
- **Threads:** `-pthread` (GCC/Clang) or `Threads::Threads` (CMake)

## Known limitations

- **POSIX-only.** Windows support would need Winsock2 (`<winsock2.h>`,
  `WSAStartup`, `closesocket` instead of `close`). The abstraction is
  contained to `http::get`, so a Windows port is a small, well-scoped
  change — but it's not implemented here.
- **Free-tier API semantics.** The API re-rolls its pool on every call.
  Deduplication and the tiered-fetch design mitigate this; they don't
  eliminate it. A paid API key or Open Trivia DB would fix it at the
  source.
- **No HTTPS.** The client uses plain HTTP on port 80. The Trivia API
  serves over HTTP, so this works — but it would not be acceptable for
  a project that handles credentials or sensitive data.

## Roadmap

- [ ] Windows support via Winsock2 (contained to `http::get`)
- [ ] Switch to Open Trivia DB for free session tokens (restores live
      fetching at all question counts)
- [ ] HTTPS support (needs OpenSSL / a TLS library — would break the
      zero-dependency promise, so it's a deliberate trade-off)
- [ ] Persist high score / session results to disk
- [ ] Category filtering in the local fallback bank

## Notes

- The binary is named `quiz`. Both the `g++` command and the CMake build
  produce it at the project root.
- The header brief in `main.cpp` explains the tiered-fetch design in
  more detail than the README — the two are meant to be read together.

## License

MIT.
