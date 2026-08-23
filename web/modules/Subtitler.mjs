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

  fonts() {
    return this.get("fonts");
  }

  state() {
    return this.get("subtitle-state");
  }

  setPaused(paused) {
    return this.put("subtitle-state", { paused: String(paused) });
  }

  setVisible(visible) {
    return this.put("subtitle-state", { visible: String(visible) });
  }

  setTime(time) {
    return this.put("subtitle-state", { time });
  }

  setFontFamily(font_family) {
    return this.put("subtitle-state", { font_family });
  }

  setFontSize(font_size) {
    return this.put("subtitle-state", { font_size });
  }

  setFontColor(font_color) {
    return this.put("subtitle-state", { font_color });
  }

  upload(filename, body) {
    return this.put(`subtitles/${filename}`, {}, { body });
  }
}

export { Subtitler };
