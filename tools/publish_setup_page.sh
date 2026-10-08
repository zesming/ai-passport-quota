#!/usr/bin/env bash
# Add the setup page to a checkout of the gh-pages branch and commit it.
#
#   tools/publish_setup_page.sh <site-dir> [page] [header]
#
# The page goes to <site-dir>/p<protocol>/index.html. It also becomes the latest page,
# <site-dir>/index.html, unless the latest page already speaks a newer protocol (a fix of an older
# protocol must not take the root back). Pages of other protocols stay where they are: an older
# Passport keeps its matching page at /p<N>/. The caller pushes.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
site="${1:?usage: $0 <site-dir> [page] [header]}"
page="${2:-${repo_root}/main/setup_page.html}"
header="${3:-${repo_root}/main/quota_portable.h}"

protocol="$(sed -n 's/^#define QUOTA_PROTOCOL_VERSION \([0-9][0-9]*\)$/\1/p' "${header}")"
if [[ -z "${protocol}" ]]; then
    echo "ERROR: QUOTA_PROTOCOL_VERSION not found in ${header}" >&2
    exit 1
fi

mkdir -p "${site}/p${protocol}"
cp "${page}" "${site}/p${protocol}/index.html"
latest="$(cat "${site}/.latest-protocol" 2>/dev/null || echo 0)"
if (( protocol >= latest )); then
    cp "${page}" "${site}/index.html"
    echo "${protocol}" > "${site}/.latest-protocol"
fi
touch "${site}/.nojekyll"

git -C "${site}" add -A
if git -C "${site}" diff --cached --quiet; then
    echo "The published pages are already up to date."
else
    git -C "${site}" -c user.name="github-actions[bot]" \
        -c user.email="41898282+github-actions[bot]@users.noreply.github.com" \
        commit --quiet -m "Publish the setup page for protocol ${protocol}"
    echo "Committed /p${protocol}/ and the latest page."
fi
