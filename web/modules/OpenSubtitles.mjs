import { Rest } from "./Rest.mjs";

// Candidates rank by a Bayesian average of their rating (#451): with no
// votes the score is the fixed prior and download count breaks the tie,
// so all-unrated result sets order as before — but any rating evidence
// overrides raw popularity. The prior is fixed rather than derived from
// downloads because the download order is the corrupted signal this
// fixes.
const priorVotes = 5;
const priorMean = 7;

function ratingScore({ votes, ratings }) {
  return (votes * ratings + priorVotes * priorMean) / (votes + priorVotes);
}

function compareCandidates(a, b) {
  return (
    ratingScore(b.attributes) - ratingScore(a.attributes) ||
    b.attributes.download_count - a.attributes.download_count
  );
}

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
    this.#headers = { ...this.#headers, Authorization };
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

  async search(imdb_id) {
    // The server-side order only selects the page-1 pool; the client
    // re-ranks it by rating. The appliance is English-only.
    const order_by = "download_count";
    const order_direction = "desc";
    const languages = "en";
    const response = await this.get("subtitles", {
      imdb_id,
      order_by,
      order_direction,
      languages,
    });
    response.data.sort(compareCandidates);
    return response;
  }

  async download(file_id, file_name) {
    const sub_format = "srt";
    const body = JSON.stringify({ file_id, sub_format, file_name });
    const result = await this.post("download", {}, { body });
    const response = await window.fetch(result.link);
    if (!response.ok) {
      throw new Error(
        `GET ${result.link} failed: ${response.status} ${response.statusText}`,
      );
    }
    return response.text();
  }
}

export { OpenSubtitles };
