class Rest {
  #base = null;

  constructor(base) {
    this.#base = base;
  }

  async api(endpoint, method, query = {}, options) {
    // Sorted on keys: OpenSubtitles caches on the raw query string.
    const search = new URLSearchParams(Object.entries(query).sort());
    const url = new URL(`${endpoint}`, this.#base);
    url.search = search;
    const response = await window.fetch(url, { method, ...options });
    if (!response.ok) {
      throw new Error(
        `${method} ${url} failed: ${response.status} ${response.statusText}`,
      );
    }
    return response.json();
  }

  get(endpoint, ...args) {
    return this.api(endpoint, "GET", ...args);
  }

  put(endpoint, ...args) {
    return this.api(endpoint, "PUT", ...args);
  }

  post(endpoint, ...args) {
    return this.api(endpoint, "POST", ...args);
  }
}

export { Rest };
