// https://docs.expo.dev/guides/using-eslint/
// Lint and formatting for the phone app's TypeScript (issue #13). Prettier runs as an ESLint
// rule, so `npm run lint` / `npm run format` (repository root) cover both.
const { defineConfig } = require('eslint/config');
const expoConfig = require('eslint-config-expo/flat');
const prettierRecommended = require('eslint-plugin-prettier/recommended');

module.exports = defineConfig([
  expoConfig,
  prettierRecommended,
  {
    // Evaluated by Node (Expo CLI, ESLint, Babel, Metro), not in the app
    files: ['*.config.js'],
    languageOptions: { globals: { __dirname: 'readonly', __filename: 'readonly', process: 'readonly' } },
  },
  {
    ignores: ['dist/*', 'android/*', 'modules/mobcam-stream/android/*'],
  },
]);
