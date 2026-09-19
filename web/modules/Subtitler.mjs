import { Rest } from "./Rest.mjs";

class Subtitler extends Rest {
  onPersistenceFailure = null;

  async api(...args) {
    try {
      return await super.api(...args);
    } catch (error) {
      if (error.body?.applied === true && error.body?.persisted === false) {
        try {
          await this.onPersistenceFailure?.(error);
        } catch {
          // Preserve the original operation's error if refreshing fails.
        }
      }
      throw error;
    }
  }

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
    return this.put(`subtitles/${encodeURIComponent(filename)}`, {}, { body });
  }

  download(file) {
    return this.get(`subtitles/${encodeURIComponent(file)}`);
  }

  remove(file) {
    return this.delete(`subtitles/${encodeURIComponent(file)}`);
  }

  whisper() {
    return this.get("whisper");
  }

  setWhisper(params) {
    return this.put("whisper", params);
  }

  uploadWhisperModel(name, body) {
    return this.put(`whisper/models/${encodeURIComponent(name)}`, {}, { body });
  }

  removeWhisperModel(name) {
    return this.delete(`whisper/models/${encodeURIComponent(name)}`);
  }

  subtitleSync() {
    return this.get("subtitle-sync");
  }

  startSubtitleSync(model) {
    return this.put("subtitle-sync", model ? { model } : {});
  }
}

export { Subtitler };
