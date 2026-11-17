#!/bin/sh
# Use the commit hash even when this checkout has release tags.
commit=$(git rev-parse --short=8 HEAD 2>/dev/null) || {
    printf '%s' UNKNOWN
    exit 0
}
if ! git diff-index --quiet HEAD -- 2>/dev/null; then
    commit="${commit}-dirty"
fi
printf '%s' "$commit"
