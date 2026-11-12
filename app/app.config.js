// Extends app.json with build information so the app can show exactly which build is running:
// the git commit it was built from, and whether the working tree had uncommitted changes.
// Evaluated when the JS bundle is built (expo run:android / release build), so it is baked in.
const { execSync } = require('child_process');

function git(args) {
  try {
    return execSync(`git ${args}`, { cwd: __dirname, stdio: ['ignore', 'pipe', 'ignore'] }).toString().trim();
  } catch {
    return '';
  }
}

module.exports = ({ config }) => ({
  ...config,
  extra: {
    ...config.extra,
    commit: git('rev-parse --short HEAD'),
    modified: git('status --porcelain').length > 0,
  },
});
