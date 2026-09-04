# Improving Subtitler Auto-Sync Matching

## Current assessment

The shared-clock and synchronization-session architecture is sound. The most
likely reason auto-sync succeeds only about half the time is that the matcher
requires too much exact agreement from ordinary Whisper output:

- every audio window is a disjoint five-second block;
- matching requires four exact word trigrams at one exact word offset;
- inserted or omitted words shift subsequent trigrams into different offset
  buckets;
- the transcript is timestamped at the end of the complete audio window rather
  than at the end of the recognized speech.

The goal should remain: **a failed match is preferable to a confident but
incorrect match**. Improvements should raise the lock rate without weakening
ambiguity rejection.

## Recommended implementation order

1. Add end-of-session diagnostic summaries and collect real failures.
2. Add 250–500 ms of overlap between Whisper windows.
3. Timestamp each transcript using Whisper's final recognized segment.
4. Add matcher tests covering realistic recognition errors.
5. Replace exact word-offset voting with weighted local sequence alignment.
6. Tune thresholds using recorded data rather than synthetic examples alone.

## 1. Measure the failure modes

Classify a representative set of real attempts into:

- no usable transcript;
- insufficient matching evidence;
- ambiguous best match;
- enough votes, but offsets too widely dispersed;
- successful lock with an inaccurate position.

Record these metrics:

- lock rate;
- false-lock rate;
- median time to lock;
- absolute synchronization error.

Add one diagnostic summary when a session finishes, for example:

```text
sync failed: 9 windows, 5 voted, best cluster 2,
spread 3180 ms, rejected windows: 2 weak, 2 ambiguous
```

This will show whether failures originate in transcription, text matching, or
timestamp clustering.

## 2. Easy win: overlap adjacent audio windows

The existing five-second windows do not overlap. Words crossing a boundary can
therefore be damaged in both adjacent transcriptions.

Retain approximately 250–500 ms of the previous window:

```cpp
constexpr auto kOverlapSamples = kSampleRate / 2;

window.erase(window.begin(),
             window.begin() + kWindowSamples - kOverlapSamples);
```

A 500 ms overlap adds roughly 10% more inference work. Reset the retained audio
whenever a new auto-sync session starts so separate attempts cannot share old
audio.

The official whisper.cpp streaming example uses retained audio for the same
reason: mitigating word-boundary errors.

## 3. Easy win: use the recognized speech timestamp

The current implementation assigns the transcript the PTS of the end of the
entire five-second audio window. If the last spoken word is followed by
silence, every resulting offset includes that silence as an error.

Enable Whisper timestamps without printing them:

```cpp
params.no_timestamps = false;
params.print_timestamps = false;
```

Read the final segment's end time:

```cpp
const auto segment_end =
    whisper_full_get_segment_t1(context, segments - 1);
```

Convert Whisper's relative timestamp into the shared capture timeline:

```cpp
speech_end = window_start_pts + segment_end * 10ms;
```

Use `speech_end` in `TimestampedText`. This should reduce systematic lag after
trailing silence and tighten the two-second vote cluster.

## 4. Improve the existing trigram matcher conservatively

Do not simply lower `kMinHits`. That would improve recall by accepting more
false evidence as well.

Instead:

- give rare trigrams more weight than common trigrams;
- ignore or heavily discount trigrams occurring repeatedly in the SRT;
- combine matching word-offset buckets within approximately one word;
- require two strong, rare anchors rather than four unweighted hits;
- retain the runner-up margin and the requirement for several
  time-consistent windows.

A possible inverse-frequency weight is:

```text
weight(ngram) = log((total_ngrams + 1) / (occurrences + 1))
```

Combining neighbouring offsets allows evidence before and after a single
inserted or omitted word to support the same candidate region.

## 5. Preferred matcher: weighted local sequence alignment

The cleaner long-term solution is local word-sequence alignment, such as
Smith–Waterman, between each normalized transcript window and the flattened SRT
word stream.

Suggested scoring properties:

- matching words receive a positive score;
- substitutions receive a small penalty;
- insertions and deletions receive a small gap penalty;
- rare matching words receive more weight;
- common words receive little weight.

For a transcript of roughly 10–30 words and an SRT containing 10,000–20,000
words, a full dynamic-programming pass is only a few hundred thousand cells per
window. This is negligible compared with Whisper inference and can use linear
memory.

Local alignment directly handles cases such as:

```text
SRT:     you really do not want to go there
Whisper: you really don't    want to go there
```

Recover both the best alignment and the best spatially distinct runner-up.
Accept a window only when:

- enough of the transcript is covered;
- the normalized alignment score is sufficiently high;
- the best location clearly beats the runner-up.

Continue requiring multiple independent windows to produce compatible clock
offsets. Local alignment should improve recall; the cross-window agreement
continues to protect against false locks.

## 6. Extend the tests

The existing corruption test substitutes words while preserving their count.
Add cases for:

- an omitted word;
- an inserted word;
- repeated words;
- a leading or trailing hallucination;
- contractions and split words;
- one to three seconds of trailing silence;
- speech crossing a five-second boundary;
- accessibility annotations such as `[MUSIC]` and `(SIGHS)`;
- two similar scenes in the same subtitle file.

Also retain actual Whisper transcripts from successful and failed attempts as
matcher fixtures. Real output will expose failure modes that synthetic unique
word sequences cannot model.

## Expected payoff

The two immediate improvements are:

1. **Window overlap**, improving transcription around boundaries.
2. **Final-segment timestamps**, improving offset consistency around silence.

Weighted local alignment is the change most likely to raise the success rate
substantially beyond 50%, because it tolerates the insertions and deletions
produced by real speech recognition instead of only relaxing an exact-match
threshold.

## Reference

- [whisper.cpp streaming example](https://github.com/ggml-org/whisper.cpp/blob/master/examples/stream/stream.cpp)
