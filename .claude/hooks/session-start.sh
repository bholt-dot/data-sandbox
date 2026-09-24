#!/usr/bin/env bash
# Ensures the beans issue tracker is available and primes the agent with its usage guide.
# Installs only in remote (Claude Code on the web) containers; locally it just primes if beans exists.
set -uo pipefail

BEANS_VERSION="v0.4.2"
GOBIN_DIR="$(go env GOPATH 2>/dev/null || echo "$HOME/go")/bin"
export PATH="$PATH:$GOBIN_DIR"

if ! command -v beans >/dev/null 2>&1 && [ "${CLAUDE_CODE_REMOTE:-}" = "true" ]; then
  if command -v go >/dev/null 2>&1; then
    go install "github.com/hmans/beans@${BEANS_VERSION}" >&2 \
      || echo "session-start: failed to install beans ${BEANS_VERSION}" >&2
  else
    echo "session-start: go not found; cannot install beans" >&2
  fi
fi

# Persist PATH for the rest of the session's Bash calls.
if [ -n "${CLAUDE_ENV_FILE:-}" ]; then
  echo "export PATH=\"\$PATH:$GOBIN_DIR\"" >> "$CLAUDE_ENV_FILE"
fi

# stdout from a SessionStart hook is added to the agent's context.
if command -v beans >/dev/null 2>&1; then
  cd "${CLAUDE_PROJECT_DIR:-.}" && beans prime
fi
exit 0
