# Subtitle sync matching

Findings from running real whisper captures through the sync matcher, and the
resulting improvement plan. Reproduce anything here with `subtitler-test`
against the examples in `fragments/`.

## Current design (#18/#21/#290/#433)

- The whisper tap transcribes **disjoint 5 s windows**; each window's
  `TimestampedText` is stamped with the **end of the complete audio window**
  (the last buffer's PTS+duration, `RunWhisper` in `src/stream/stream.cpp`).
- Each window votes for a word-offset region in the flattened SRT word
  stream by **rarity-weighted** word-trigram hits (1/occurrences each;
  buckets within ±1 word merge); a window votes only when the winning
  region scores **≥ 2.0** (two unique anchors) with a **2x margin** over
  the runner-up. (Originally a flat kMinHits = 4 — see the experiment
  below for why that changed.)
- θ locks when **≥ 3 windows** vote within a **2 s spread** (median of the
  tightest run). Design rule: **fail loudly, never lock wrong** — a failed
  match is always preferable to a confident wrong one.
- Music/noise is filtered twice before the matcher: `suppress_nst = true` in
  the transcriber (drops `♪`, `(music)`, `[BLANK_AUDIO]`, ...), and
  `NormalizeMatchWords` keeps only alphanumerics, so any surviving `♪`
  normalizes to zero words and the window is inert. Music handling needs no
  further work.

## The Cigarette Burns experiment

`fragments/CigaretteBurns.md` holds three real capture fragments (45 s
deadline ≈ 9 windows each) plus their expected cues. Run them with:

```sh
build/subtitler-test "fragments/Masters of Horror - S01E08  Cigarette Burns.os2055124.25fps.tt0643109.srt" --windows=<fragment-windows-file>
```

Results with the original matcher:

| Fragment | Outcome | Why |
|---|---|---|
| 1 (music-heavy) | **no lock** (0 of 8 voted) | 6 windows are `♪♪` (inert). "There you are" → 1 hit, "son of a bitch." → 2 hits, both **below kMinHits** — yet both trigrams are *unique in the whole SRT* (runner-up 0). |
| 2 (dialogue) | locked, 3 of 4 voted | Correct despite whisper hearing "Mr. Bowne" for "Mr. Bellinger". **But the vote cluster had only 129 ms of slack** against the 2 s spread (votes spread 1.87 s). |
| 3 (dialogue) | locked, 3 of 6 voted | Correct despite "found to the negative" for "bound to the negative". Music/"Oh!" windows inert. |

After step 1 below (frequency weighting): fragments 2 and 3 lock with
**identical θ** as before; fragment 1's "son of a bitch." votes with score
2.00 (two unique anchors, θ lands exactly on cue 101's end) and "There you
are" stays below threshold by design — fragment 1 still can't lock with 1 of
8 windows voting (root cause 4, session policy). Median `MatchTranscript`
cost on this 654-cue SRT: **1.7 ms** (dev machine), unchanged from before
the weighting.

## Root causes

1. **Frequency blindness.** The flat hit threshold treats a trigram occurring
   once in the movie the same as one occurring fifty times. Short windows
   whose trigrams are unique (runner-up 0 — the strongest evidence the
   matcher can see) are discarded, e.g. fragment 1's "There you are".
2. **Timestamp quantization.** A vote stamps the window's *end* onto its
   last word, but with 5 s windows the last word can sit anywhere inside —
   correct votes carry up to ~5 s of noise against a 2 s cluster tolerance.
   Fragment 2 locked by luck of alignment; fragment 1's two would-be votes
   compute to 3.6 s apart (cue 101's bounds: "are" at 2/7 through the cue vs
   "bitch" at the end), which the cluster would have rejected as scattered.
3. **Boundary clipping.** Disjoint windows damage words crossing a boundary
   in both adjacent transcripts (fragment 3's "sold to flesh" for "like soul
   to flesh" starts exactly at a window edge).
4. **Session policy.** Fragment 1 has only 2 speech-carrying windows in 40 s
   of audio — below kMinVotes = 3 no matter how good the matcher is. The
   45 s deadline can cut a session off mid-scene while evidence is still
   accumulating.

## Improvement plan

Ordered; each step must keep "never lock wrong" — the 3-vote / 2 s cluster
remains the primary guard, so per-window gates may relax only where the
evidence they accept is *stronger*, not weaker.

### 1. Frequency-weighted voting (matcher-local, **done**)

- Weight each trigram hit by **1/occurrences** in the SRT (a unique trigram
  scores 1.0, one occurring ten times 0.1).
- Replace kMinHits with a **strength threshold of 2.0** — "two strong rare
  anchors" instead of four unweighted hits. The 2x margin applies to
  weighted scores.
- **Merge offset buckets within ±1 word** before picking the winner, so one
  inserted/dropped word doesn't split a window's hits into two offsets; θ
  comes from the strongest raw bucket inside the winning region.
- `WindowMatch` keeps the raw hit counts for tooling and exposes the
  weighted scores; decisions use scores only.
- Under this rule fragment 1's "son of a bitch." (2 unique anchors) votes;
  "There you are" (1 anchor) still doesn't — accepted, see root cause 4.

### 2. Speech-end timestamps (transcriber + stream)

Root-cause fix for root cause 2: set `no_timestamps = false`, read the last
segment's end (`whisper_full_get_segment_t1`, centiseconds from window
start) and stamp the window with `window_end − 5 s + t1 × 10 ms` instead of
the bare window end. Shrinks correct-vote noise from ~5 s to sub-second,
keeping the 2 s cluster strict — strictly better than widening the spread.
Verify with `SUBTITLER_TEST_WHISPER_MODEL`/`_WAV` and on the Pi.

### 3. Window overlap (transcriber)

Retain 250–500 ms of the previous window instead of erasing everything after
inference (~10% more inference for 500 ms; whisper.cpp's stream example does
the same). Interacts with step 2's window-start bookkeeping, so it comes
after. Reset retained audio when a new sync session starts.

### 4. Session-end diagnostics

One `sync:info` summary per finished session — windows, votes, best cluster
size and spread, reject breakdown — so real captures can be classified (no
transcript / weak evidence / ambiguous / scattered / inaccurate lock) and
the thresholds tuned on data rather than synthetic examples.

### 5. Corruption tests, then judge alignment

Extend the matcher tests: inserted/omitted/repeated words, leading/trailing
hallucinations, contractions, trailing silence, boundary-crossing speech,
`[MUSIC]`/`(SIGHS)` annotations, two similar scenes. Keep real captures as
fixtures. Only if weighted trigrams still underperform against that corpus,
evaluate weighted local sequence alignment (Smith–Waterman over the word
streams; a few hundred thousand DP cells per window — see
`subtitler_matching_improvements.md`).

## Open questions

- **Session policy for sparse scenes.** Even a perfect matcher can't lock
  fragment 1: 2 speech windows < 3 votes. Either a strength-aware vote
  count (should two rare-anchor votes suffice?) or a deadline that extends
  while the top cluster keeps growing. Deliberately *not* decided here —
  it's a behavior change, not a matching-quality change.
- **Vote safety after weighting.** The threshold stops being the primary
  false-lock defense; the cluster takes over. Acceptable because 3
  independent wrong votes agreeing within 2 s is implausible, but worth
  revisiting if diagnostics (step 4) show near-misses.

## Performance budget

Live, `SyncSession::Feed` runs the full matcher once per whisper window
(every ~5 s, worst case ~9 accumulated windows at the deadline), and
`MatchTranscript` re-indexes the whole SRT (~10–20k words) per call. The
quality work above only adds per-trigram weight lookups, so the budget
question is empirical. `subtitler-test --benchmark[=<n>]` (default 100)
repeats `MatchTranscript` over the given windows and reports min/median/max
per call — the number to watch as the matcher grows (it must stay far below
the 5 s window cadence, on the Pi, not just the dev machine).

## References

- `fragments/CigaretteBurns.md` — the experiment data.
- `subtitler_matching_improvements.md` — the parallel analysis this plan
  merges (window overlap, segment timestamps, alignment sketch).
- Issues #18 (matcher seam), #21 (trigram voting), #290 (normalization),
  #433 (one-shot session).
- [whisper.cpp stream example](https://github.com/ggml-org/whisper.cpp/blob/master/examples/stream/stream.cpp) — overlap precedent.
