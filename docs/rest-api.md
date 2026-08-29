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
usable title is non-empty, does not start with a dot, contains no slashes,
and ends in `.srt`.

### PUT /api/subtitles/<title>

Uploads an SRT: the percent-decoded `<title>` is validated, the raw request
body is stored in the library, marked `active` for boot resume, and
switched in live (re-anchored at the current running time).

- `201 Created` — body is the library-relative name as JSON:
  `{"stored_name":"m/Movie.srt"}`
- `400 Bad Request` — invalid title, bad percent-encoding, or empty body
- `405 Method Not Allowed` — anything but GET or PUT
- `413 Content Too Large` — body over 8 MiB
- `500 Internal Server Error` — storage or live activation failed

```js
const response = await window.fetch("/api/subtitles/Movie.srt", {
  method: "PUT",
  body: srtText,
});
const { stored_name } = await response.json();
```

### GET /api/subtitles/<title>

The stored SRT for the percent-decoded `<title>`, as JSON:

```json
{
  "body": "1\n00:00:00,200 --> 00:00:01,400\nHello subtitles\n"
}
```

- `400 Bad Request` — bad percent-encoding
- `404 Not Found` — no such library title

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
  `false` shows them again and resumes from the frozen position.
- `time` — SRT position in milliseconds; works paused (moves the frozen
  position) or playing. `0` restarts from the beginning.
- `delay` — live trim in milliseconds; positive delays cues.
- `visible` — show/hide without disturbing the subtitle branch.
- `font_family` — cue font family, one of `GET /api/fonts`.
- `font_size` — cue font size in points; a positive integer.
- `font_color` — cue color as `#rrggbb` (always opaque). The `#` must be
  percent-encoded (`%23`) in the query string.

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

`model` is the model in use (its store file name), or `null` when none
was ever enabled. 404 when the hooks are unset.

### PUT /api/whisper

Changes the state via query parameters; answers the state like GET:

- `enabled` — `true` starts transcription, `false` stops it (the tap's
  gate closes and the model unloads). Enabling needs a model: the
  current one, or the one named by `model`.
- `model` — selects the ggml model: a file name from `models`. Applies
  live, also while disabled (loaded on the next enable).

400 on invalid values (unknown parameters, a malformed or unstored
model name, enabling with no model); 405 for methods other than
GET/PUT.

### PUT /api/whisper/models/<name>

Stores a model in the state dir's `models/`: the percent-decoded
`<name>` must end in `.bin` with no slashes or leading dot; the body is
the ggml file, written to `<name>` via a temp file + rename.

```js
await window.fetch("/api/whisper/models/ggml-tiny.en.bin", {
  method: "PUT",
  body: ggmlBytes,
});
```

- `201 Created` — `{"stored_name":"ggml-tiny.en.bin"}`
- `400 Bad Request` — invalid name or empty body
- `413 Content Too Large` — body over 512 MiB (libsoup reads the whole
  body before the handler runs, so the cap also bounds that transient)

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
