# REST API

The appliance web server (`--web`, port 8080) exposes the endpoints below.
They are building blocks: the web interface (#15) composes complex behavior
in JavaScript, and the server exposes only primitives. Response JSON is
hand-rolled; state-changing input rides on query parameters, which libsoup
hands to the handler already parsed.

## Preview

The MJPEG preview shows the composited video (subtitles included — the tee
sits after the overlay). JPEG encoding runs only while at least one MJPEG
client is connected; until the first frame is encoded, both endpoints serve
a magenta placeholder. The preview is not an authoritative synchronization
display — see the timing limitation in docs/video-output.md.

### GET /api/preview.jpg

The newest encoded frame.

```http
200 OK
Content-Type: image/jpeg
Cache-Control: no-store
X-Frame-Sequence: <sequence number>
```

503 only if the placeholder seeding failed at startup.

### GET /api/preview.mjpeg

A multipart stream, newest-frame-only per client.

```http
200 OK
Content-Type: multipart/x-mixed-replace; boundary=frame
Cache-Control: no-store
Pragma: no-cache
```

Each part:

```http
--frame
Content-Type: image/jpeg
Content-Length: <number of bytes>
X-Frame-Sequence: <sequence number>

<JPEG data>
```

Slow clients skip frames rather than building a backlog (one frame in
flight, one pending). At most 4 concurrent clients; 503 beyond that. The
first client starts the encoder, the last disconnect stops it.

## Subtitles

Subtitles live in the state-dir library, sharded by the title's first
letter lowercased (`_` for non-letters): `subtitles/<bucket>/<title>`. A
usable title is non-empty, at most 255 bytes, does not start with a dot,
contains no slashes or control characters, and ends in `.srt`. Titles
travel exactly once-encoded: the client percent-encodes the path
segment and the server decodes it once.

### PUT /api/subtitles/<title>

Uploads an SRT: the `<title>` is validated, the request body is
stored in the library, marked `active` for boot resume, and switched
in live (re-anchored at the current running time). The body streams
into a staged file rather than memory. Storing is separate from
selecting: the marker and the persisted configuration change only
after the live switch succeeds, and a failed switch restores the
previous playback (an SRT switch never restarts capture).

- `201 Created` — body is the library-relative name as JSON:
  `{"stored_name":"m/Movie.srt"}`
- `400 Bad Request` — invalid title or empty body
- `405 Method Not Allowed` — anything but GET, PUT, or DELETE
- `413 Content Too Large` — body over 8 MiB (rejected at the declared
  length, or while streaming when the cap is crossed)
- `500 Internal Server Error` — storage or live activation failed

```js
const response = await window.fetch("/api/subtitles/Movie.srt", {
  method: "PUT",
  body: srtText,
});
const { stored_name } = await response.json();
```

### GET /api/subtitles/<title>

The stored SRT for `<title>`, as JSON:

```json
{
  "body": "1\n00:00:00,200 --> 00:00:01,400\nHello subtitles\n"
}
```

- `404 Not Found` — no such library title

### DELETE /api/subtitles/<title>

Removes `<title>` from the library. Deleting the attached subtitle
detaches it (like `PUT /api/subtitle-state?file=`) and clears the
`active` marker. The entry is removed first, so a failed removal
changes nothing — the selection stays usable.

```js
await window.fetch("/api/subtitles/Movie.srt", { method: "DELETE" });
```

- `204 No Content` — deleted
- `400 Bad Request` — missing title
- `404 Not Found` — no such library title
- `500 Internal Server Error` — removal or live detach failed

### GET /api/subtitles

The library titles as a JSON array, scanned from the state dir on
demand:

```json
["Movie.srt", "Show S01E01.srt"]
```

### GET /api/subtitle-state

The live subtitle state; `time` is computed fresh from the running
time:

```json
{
  "file": "Movie.srt",
  "visible": true,
  "paused": false,
  "time": 73400,
  "delay": 0,
  "font_family": "Sans",
  "font_size": 24,
  "font_color": "#ffd700"
}
```

`file` is the active library title, or `null` when no subtitles are
attached. `time` is the current SRT position in milliseconds, `delay` the
live trim in milliseconds (positive delays cues). `font_family`,
`font_size` (points), and `font_color` are the cue style, `null` when
never set — the renderer default.

At startup the attached subtitle comes from `--subtitles`, then the
config's `[subtitles] file`, then the state dir's `active` marker, in
that precedence order. A restored path that names a library entry
reports `file` as its title; a path outside the library reports `null`.

### PUT /api/subtitle-state

Changes any subset of the state via query parameters:

```js
await window.fetch("/api/subtitle-state?paused=true", { method: "PUT" });
await window.fetch("/api/subtitle-state?time=0", { method: "PUT" });
await window.fetch("/api/subtitle-state?file=Show%20S01E01.srt", { method: "PUT" });
```

- `file` — a title from `GET /api/subtitles`; resolved via the library and
  marked `active` for boot resume. Empty detaches subtitles entirely.
- `paused` — `true` hides the subtitles and freezes the SRT position;
  `false` shows them again and resumes from the frozen position. Pause
  and visibility compose: rendering is off while paused OR hidden.
- `time` — SRT position in milliseconds; works paused (moves the frozen
  position) or playing. `0` restarts from the beginning. Negative seeks
  before the start. Bounded at ±2305843009213 ms (~73 years).
- `delay` — live trim in milliseconds; positive delays cues, negative
  advances them. Same bound as `time`.
- `visible` — show/hide without disturbing the subtitle branch.
- `font_family` — cue font family, one of `GET /api/fonts`.
- `font_size` — cue font size in points, 1–1000.
- `font_color` — cue color as `#rrggbb` (always opaque). The `#` must be
  percent-encoded (`%23`) in the query string.

The persisted `[subtitles]` configuration follows successful changes
as a best-effort write-back: a write failure is logged, not reported,
and the live state stands.

### PUT /api/subtitle-sync

Starts (or restarts) a one-shot auto-sync session (#433): the appliance
listens to the capture audio for up to ~45 seconds, matches the whisper
transcript against the attached SRT, and on a stable match jumps the
SRT clock to the matched position. Needs subtitles attached and capture
running. The session owns its transcription: when the tap is off, it
enables it with the model named by `model` and releases it when the
session ends, however it ends — a temporary activation that is never
persisted; when the tap is already on, the session rides it and changes
nothing.

```js
await window.fetch("/api/subtitle-sync?model=ggml-tiny.en.bin", { method: "PUT" });
```

- `model` — a model file name from `GET /api/whisper`; required when
  the tap is off, ignored while it runs.

- `202 Accepted` — listening: `{"state":"listening"}`
- `400 Bad Request` — unknown parameters or an empty `model`
- `409 Conflict` — can't start, with the reason:
  `{"state":"failed","reason":"no subtitles attached"}`,
  `{"state":"failed","reason":"capture isn't running"}`,
  `{"state":"failed","reason":"whisper is disabled"}`,
  `{"state":"failed","reason":"the model isn't available"}`, or
  `{"state":"failed","reason":"the subtitle file can't be parsed"}`

### GET /api/subtitle-sync

The session state:

```json
{ "state": "listening" }
```

`state` is `idle` (no session, or cancelled by a subtitle switch,
manual seek, or whisper toggle), `listening`, `synced` (with
`"time": <matched SRT position at lock time in ms>`), or `failed` (with
`"reason"`). The film must actually be playing for a lock; a session
that finds no stable match within the listening window fails.

### GET /api/fonts

The font families the subtitle renderer can use, as a JSON array:

```json
["Cantarell", "DejaVu Sans", "DejaVu Serif"]
```

## Whisper

The whisper tap (#19) transcribes the capture audio with whisper.cpp
and logs the text (`SUBTITLER_LOG=stream:info`) — the groundwork for
whisper-based features like auto-sync. Models are ggml files in the
state dir's `models/` store; they are never downloaded automatically —
the web interface fetches them from HuggingFace in the browser (CORS
allows it) and stores them through this API. The state endpoints need
their hooks; model listing and storage additionally need a state dir.

### GET /api/whisper

The live whisper state and the stored models:

```json
{
  "enabled": false,
  "model": "ggml-tiny.en.bin",
  "models": ["ggml-tiny.en.bin"]
}
```

`model` is the selected model (its store file name), or `null` when
none was ever selected. The selection survives reboot independently of
`enabled`: at startup the configured model is restored whether or not
transcription starts. 404 when the hooks are unset.

### PUT /api/whisper

Changes the state via query parameters; answers the state like GET:

- `enabled` — `true` starts transcription, `false` stops it (the tap's
  gate closes and the model unloads). Enabling needs a model: the
  current one, or the one named by `model`. Enabling while an auto-sync
  session owns the tap transfers the ownership: the tap stays on when
  the session ends.
- `model` — selects the ggml model: a file name from `models`. Applies
  live, also while disabled (loaded on the next enable).

400 on invalid values (unknown parameters, a malformed or unstored
model name, enabling with no model); 405 for methods other than
GET/PUT.

### PUT /api/whisper/models/<name>

Stores a model in the state dir's `models/`: `<name>` must end in
`.bin` with no slashes, control characters, or leading dot; the body is
the ggml file. The body streams into a staged file in the store (bounded
memory, at most 4 concurrent uploads) and is renamed over `<name>` only
once complete — a partial model is never exposed as installed.

```js
await window.fetch("/api/whisper/models/ggml-tiny.en.bin", {
  method: "PUT",
  body: ggmlBytes,
});
```

- `201 Created` — `{"stored_name":"ggml-tiny.en.bin"}`
- `400 Bad Request` — invalid name or empty body
- `413 Content Too Large` — body over 512 MiB (rejected at the declared
  length, or while streaming when the cap is crossed)
- `503 Service Unavailable` — too many concurrent uploads

### DELETE /api/whisper/models/<name>

Removes a model from the store. Deleting the selected model clears the
selection (and its persistence) server-side.

```js
await window.fetch("/api/whisper/models/ggml-tiny.en.bin", {
  method: "DELETE",
});
```

- `204 No Content` — removed
- `400 Bad Request` — invalid name
- `404 Not Found` — no such model stored
- `409 Conflict` — the tap is currently running this model:
  `{"reason":"model in use"}` (a selected-but-disabled model can be
  deleted; the selection is then cleared)

### GET /api/whisper/transcript

A server-sent-events stream with one event per transcribed whisper
window — the transcript as it happens, for matcher development:

```
data: {"timestamp_ns":123456789000,"text":"the lazy brown fox"}
```

`timestamp_ns` is the running time the window's audio ended at (the
shared capture/output timeline). The web interface's capture log
appends each event as a `<timestamp-ns>\t<text>` line — the verbatim
`--windows` input of `subtitler-test`. Max four clients; 503 beyond.
Windows arrive only while the tap is enabled; the stream stays silent
otherwise.

## OpenSubtitles

### GET /api/opensubtitles

The OpenSubtitles API key given with `--api-key`, as JSON:

```json
{
  "api_key": "..."
}
```

404 when no key is configured; 405 for anything but GET.

## Static files

GET paths not claimed by a registered route fall back to the web root
(`/` maps to `index.html`). Only `.html`, `.js`, `.mjs`, `.css`, and `.png` are
served — the allowlist doubles as the MIME map — and anything else,
non-GET methods, and traversal attempts are 404.
