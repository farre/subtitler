import { Rest } from "./Rest.mjs"

class OpenSubtitles extends Rest {
    #headers = {};
    #authenticated = false;

    constructor(base, apiKey) {
        super(base);

        this.#headers = {
            "Content-Type": "application/json",
            "Api-Key": apiKey,
            "User-Agent": "subtitler v0.1.0",
            Accept: "application/json",
        };
    }

    get authenticated() {
        return this.#authenticated;
    }

    addBearerToken(token) {
        const Authorization = `Bearer ${token}`;
        this.#headers = { Authorization, ...this.#headers };
        this.#authenticated = true;
    }

    api(endpoint, method, query = {}, { headers = [], ...properties } = {}) {
        headers = { ...this.#headers, ...headers };
        return super.api(endpoint, method, query, { headers, ...properties });
    }

    async login(username, password) {
        const body = JSON.stringify({ username, password });
        const result = await this.post("login", {}, { body });
        this.addBearerToken(result.token);
    }

    search(imdb_id) {
        const languages = "en";
        // const ai_translated = "exclude";
        const order_by = "download_count";
        const order_direction = "desc";
        return this.get("subtitles", { imdb_id, order_by, order_direction })
    }

    async download(file_id, file_name) {
        const sub_format = "srt";
        const body = JSON.stringify({ file_id, sub_format, file_name });
        const result = await this.post("download", {}, { body });
        const response = await window.fetch(result.link);
        return response.text();
    }
}

export { OpenSubtitles }