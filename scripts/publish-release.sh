#!/usr/bin/env bash
set -euo pipefail

repo=pe1itr/atv-receiver-analog
if [[ $# == 0 ]]; then
    echo "Usage: bash scripts/publish-release.sh TAG [gh release create options and assets] | --prune-only" >&2
    exit 2
fi
if [[ $# != 1 || $1 != --prune-only ]]; then
    gh release create --repo "$repo" "$@"
fi

older=$(gh api --paginate --slurp "repos/$repo/releases?per_page=100" |
    jq -r 'add | map(select(.draft == false)) | sort_by(.published_at, .id) | reverse | .[3:][] | .tag_name')
while IFS= read -r tag; do
    if [[ -n "$tag" ]]; then
        gh release delete --repo "$repo" --yes --cleanup-tag -- "$tag"
    fi
done <<< "$older"
