import { test } from "node:test";
import assert from "node:assert/strict";
import { Subtitler } from "../../web/modules/Subtitler.mjs";

test("partial persistence failure notifies the UI and remains an error", async () => {
  const body = { applied: true, persisted: false };
  globalThis.window = {
    fetch: async () =>
      new Response(JSON.stringify(body), {
        status: 500,
        headers: { "Content-Type": "application/json" },
      }),
  };
  const client = new Subtitler("http://subtitler.local/api/");
  let notifications = 0;
  client.onPersistenceFailure = (error) => {
    ++notifications;
    assert.deepEqual(error.body, body);
  };
  await assert.rejects(client.select("Movie.srt"), (error) => {
    assert.deepEqual(error.body, body);
    return true;
  });
  assert.equal(notifications, 1);
});

test("ordinary failures do not claim a partially applied change", async () => {
  globalThis.window = {
    fetch: async () => new Response("bad input", { status: 400 }),
  };
  const client = new Subtitler("http://subtitler.local/api/");
  client.onPersistenceFailure = () => assert.fail("unexpected notification");
  await assert.rejects(client.select("missing.srt"));
});
