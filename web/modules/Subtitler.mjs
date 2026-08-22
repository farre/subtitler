import { Rest } from "./Rest.mjs"

class Subtitler extends Rest {
    constructor(base) {
        super(base);
    }

    get list() {
        return this.get("subtitles");
    }

    get apiKey() {
        return this.get("opensubtitles");
    }

    set select(file) {
        return this.put("subtitle-state", { file });
    }

    upload(filename, body) {
        return this.put(`subtitles/${filename}`, {}, { body });
    }
}

export { Subtitler }