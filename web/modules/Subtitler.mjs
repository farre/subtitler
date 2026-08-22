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

  upload(filename, body) {
    return this.put(`subtitles/${filename}`, {}, { body });
  }
}

export { Subtitler };
