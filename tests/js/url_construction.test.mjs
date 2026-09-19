import { test } from "node:test";
import assert from "node:assert/strict";

// URL construction for user-controlled filename segments: every
// endpoint must encode the segment with encodeURIComponent exactly
// once (the server decodes exactly once). Runs the production client
// module against a mock fetch.

const calls = [];

globalThis.window = {
  fetch: async (url) => {
    calls.push(String(url));
    return new Response("null", {
      status: 200,
      headers: { "Content-Type": "application/json" },
    });
  },
};

const { Subtitler } = await import("../../web/modules/Subtitler.mjs");

const base = "http://subtitler.local/api/";
const subtitler = new Subtitler(base);

test("subtitle filename segments are percent-encoded", async () => {
  await subtitler.upload("Who?.srt", "body");
  assert.equal(calls.at(-1), `${base}subtitles/Who%3F.srt`);

  await subtitler.download("C# Sharp.srt");
  assert.equal(calls.at(-1), `${base}subtitles/C%23%20Sharp.srt`);

  await subtitler.remove("100%.srt");
  assert.equal(calls.at(-1), `${base}subtitles/100%25.srt`);

  await subtitler.download("a%20b.srt");
  assert.equal(calls.at(-1), `${base}subtitles/a%2520b.srt`);

  await subtitler.upload("Quote\"Test.srt", "body");
  assert.equal(calls.at(-1), `${base}subtitles/Quote%22Test.srt`);

  await subtitler.download("München.srt");
  assert.equal(calls.at(-1), `${base}subtitles/M%C3%BCnchen.srt`);

  // Ordinary names pass through untouched.
  await subtitler.download("Movie.srt");
  assert.equal(calls.at(-1), `${base}subtitles/Movie.srt`);
});

test("whisper model segments are percent-encoded", async () => {
  await subtitler.uploadWhisperModel("ggml-tiny.en.bin", "body");
  assert.equal(calls.at(-1), `${base}whisper/models/ggml-tiny.en.bin`);

  await subtitler.removeWhisperModel("a%20b.bin");
  assert.equal(calls.at(-1), `${base}whisper/models/a%2520b.bin`);
});

test("query values go through URLSearchParams, not path encoding", async () => {
  await subtitler.select("C# Sharp.srt");
  assert.equal(calls.at(-1), `${base}subtitle-state?file=C%23+Sharp.srt`);

  await subtitler.setWhisper({ model: "ggml-tiny.en.bin" });
  assert.equal(calls.at(-1), `${base}whisper?model=ggml-tiny.en.bin`);
});

test("the sync start carries the chosen model as a query value", async () => {
  await subtitler.startSubtitleSync("ggml-tiny.en.bin");
  assert.equal(calls.at(-1), `${base}subtitle-sync?model=ggml-tiny.en.bin`);

  await subtitler.startSubtitleSync(null);
  assert.equal(calls.at(-1), `${base}subtitle-sync`);
});
