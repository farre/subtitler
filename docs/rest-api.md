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

- `201 Created` — body is the library-relative name (e.g. `m/Movie.srt`)
- `400 Bad Request` — invalid title, bad percent-encoding, or empty body
- `405 Method Not Allowed` — anything but PUT
- `413 Content Too Large` — body over 8 MiB
- `500 Internal Server Error` — storage or live activation failed

```sh
curl -X PUT --data-binary @Movie.srt \
    http://subtitler:8080/api/subtitles/Movie.srt
```

### GET /api/subtitles

**Planned (#441).** The library titles as a JSON array, scanned from the
state dir on demand:

```json
["Movie.srt", "Show S01E01.srt"]
```

### GET /api/subtitle-state

**Planned (#441).** The live subtitle state; `time` is computed fresh from
the running time:

```json
{
  "file": "Movie.srt",
  "visible": true,
  "paused": false,
  "time": 73400,
  "delay": 0
}
```

`file` is the active library title, or `null` when no subtitles are
attached. `time` is the current SRT position in milliseconds, `delay` the
live trim in milliseconds (positive delays cues).

### PUT /api/subtitle-state

**Planned (#441).** Changes any subset of the state via query parameters:

```sh
curl -X PUT 'http://subtitler:8080/api/subtitle-state?paused=true'
curl -X PUT 'http://subtitler:8080/api/subtitle-state?time=0'
curl -X PUT 'http://subtitler:8080/api/subtitle-state?file=Show%20S01E01.srt'
```

- `file` — a title from `GET /api/subtitles`; resolved via the library and
  marked `active` for boot resume. Empty detaches subtitles entirely.
- `paused` — `true` freezes the SRT clock: the cue at the paused position
  stays composited, even past its out-time. `false` resumes from the
  frozen position.
- `time` — SRT position in milliseconds; works paused (moves the frozen
  position) or playing. `0` restarts from the beginning.
- `delay` — live trim in milliseconds; positive delays cues.
- `visible` — show/hide without disturbing the subtitle branch.

## Static files

GET paths not claimed by a registered route fall back to the web root
(`/` maps to `index.html`). Only `.html`, `.js`, `.css`, and `.png` are
served — the allowlist doubles as the MIME map — and anything else,
non-GET methods, and traversal attempts are 404.
