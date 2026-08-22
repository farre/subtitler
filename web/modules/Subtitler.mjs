import { Rest } from "./Rest.mjs";

class Subtitler extends Rest {
  list() {
    return this.get("subtitles");
  }

  apiKey() {
    return this.get("opensubtitles");
  }

  select(file) {
    return this.put("subtitle-state", { file });
  }

  state() {
    return this.get("subtitle-state");
  }

  setPaused(paused) {
    return this.put("subtitle-state", { paused: paused ? 1 : 0 });
  }

  setTime(time) {
    return this.put("subtitle-state", { time });
  }

  upload(filename, body) {
    return this.put(`subtitles/${filename}`, {}, { body });
  }
}

export { Subtitler };
