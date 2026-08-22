import js from "@eslint/js";
import html from "eslint-plugin-html";
import globals from "globals";

// The web assets are ES modules running in the browser; the inline
// <script type="module"> in index.html is linted via eslint-plugin-html.
const languageOptions = {
  ecmaVersion: 2022,
  sourceType: "module",
  globals: globals.browser,
};

export default [
  js.configs.recommended,
  { files: ["web/**/*.mjs"], languageOptions },
  { files: ["web/**/*.html"], plugins: { html }, languageOptions },
];
