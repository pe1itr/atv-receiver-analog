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

releases=$(gh api --paginate --slurp "repos/$repo/releases?per_page=100" |
    jq 'add // [] | map(select(.draft == false)) | sort_by(.published_at, .id) | reverse')

# Publishing can run from a separate release worktree. Update the checkout
# holding main before removing any old releases, including on --prune-only.
git fetch origin refs/heads/main:refs/remotes/origin/main --tags
latest=$(jq -r 'map(select(.prerelease == false))[0].tag_name // empty' <<< "$releases")
if [[ -n "$latest" ]] && ! git merge-base --is-ancestor "refs/tags/$latest" refs/remotes/origin/main; then
    echo "Release $latest is missing from origin/main. Integrate the release commit and rerun with --prune-only." >&2
    exit 1
fi
while IFS= read -r -d '' line; do
    case "$line" in
        "worktree "*) checkout=${line#worktree } ;;
        "branch refs/heads/main")
            if ! git -C "$checkout" merge --ff-only refs/remotes/origin/main; then
                echo "Could not update main at $checkout. Resolve the conflict and rerun with --prune-only." >&2
                exit 1
            fi
            ;;
    esac
done < <(git worktree list --porcelain -z)

older=$(jq -r '.[3:][] | .tag_name' <<< "$releases")
while IFS= read -r tag; do
    if [[ -n "$tag" ]]; then
        gh release delete --repo "$repo" --yes --cleanup-tag -- "$tag"
    fi
done <<< "$older"
