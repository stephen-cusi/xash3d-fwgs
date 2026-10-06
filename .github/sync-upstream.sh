#!/usr/bin/env bash
# Helper for the "Sync upstream" workflow.
#
# Merge policy:
# - Regular file conflicts are resolved in favour of upstream (-X theirs).
# - Submodule pointers already tracked by this fork keep the fork's pin:
#   mainui_cpp and xash3d-maintui are forked repositories, and letting
#   upstream move a gitlink to a commit the fork's submodule URL does not
#   serve would break local (WOA) builds.
# - Submodules and files that the fork deleted stay deleted: 3rdparty/mainui
#   is intentionally absent, and wscript's Subproject class skips missing
#   directories, so builds keep working.
# - Submodules newly added by upstream are taken as-is.
# - The fork's own workflow definitions (.github/workflows/) are never
#   touched by the sync: GitHub rejects pushes made with the Actions token
#   that modify workflow files, and the fork's CI matrix (including the
#   windows-11-arm WOA job) is maintained here, not upstream.
set -euo pipefail

UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/FWGS/xash3d-fwgs.git}"
UPSTREAM_BRANCH="${UPSTREAM_BRANCH:-master}"
TARGET_BRANCH="${TARGET_BRANCH:-master}"
MERGE_MESSAGE="${MERGE_MESSAGE:-chore: sync with upstream FWGS/xash3d-fwgs}"

git config user.name "github-actions[bot]"
git config user.email "github-actions[bot]@users.noreply.github.com"

if ! git remote get-url upstream >/dev/null 2>&1; then
	git remote add upstream "$UPSTREAM_URL"
fi
git fetch --no-recurse-submodules upstream "$UPSTREAM_BRANCH"

# The fork always carries its own commits, so a plain HEAD-vs-upstream SHA
# comparison would re-merge every day. Only merge when upstream has commits
# that are not yet contained in this branch.
if git merge-base --is-ancestor "upstream/$UPSTREAM_BRANCH" HEAD; then
	echo "Already up to date with upstream."
	exit 0
fi

# Remember which submodules the fork tracks and at which commits before the
# merge touches them. Fields of git ls-files -s: mode sha stage path.
PRE_GITLINKS="$(mktemp)"
git ls-files -s | awk '$1 == "160000" { print $2 "\t" $4 }' > "$PRE_GITLINKS"

if git merge --no-ff --no-commit --no-edit -X theirs "upstream/$UPSTREAM_BRANCH"; then
	:
else
	# -X theirs cannot settle delete/modify conflicts, which is exactly what
	# 3rdparty/mainui is: deleted by the fork, updated by upstream. Keep the
	# deletion (the upstream version may be left untracked in the tree, which
	# is harmless for the commit and the push).
	while IFS= read -r path; do
		[ -n "$path" ] || continue
		echo "Unresolved conflict at $path, keeping it deleted"
		git rm -r --cached --quiet -- "$path"
	done < <(git diff --name-only --diff-filter=U)
fi

# Keep the fork's own CI definitions out of the sync commit: GitHub
# rejects pushes from the Actions token that modify .github/workflows/,
# and the fork's workflow files carry fork-only jobs (windows-11-arm).
git restore --source HEAD --staged --worktree -- .github/workflows

# Restore the submodule pins the fork tracks. -X theirs would have moved them
# to upstream's commits, which may be unreachable through the fork URLs.
while IFS=$'\t' read -r sha path; do
	[ -n "$path" ] || continue
	cur="$(git ls-files -s -- "$path" | awk '$1 == "160000" { print $2; exit }')"
	if [ -z "$cur" ]; then
		git update-index --add --cacheinfo "160000,$sha,$path"
	elif [ "$cur" != "$sha" ]; then
		git update-index --cacheinfo "160000,$sha,$path"
	fi
done < "$PRE_GITLINKS"
rm -f "$PRE_GITLINKS"

git commit -m "$MERGE_MESSAGE"
git push origin "HEAD:$TARGET_BRANCH"
echo "Synced $TARGET_BRANCH with upstream/$UPSTREAM_BRANCH."
