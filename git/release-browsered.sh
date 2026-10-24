#!/bin/sh
# Tag and push a Browsered release; GitHub Actions then builds Linux, macOS
# and Windows and publishes the GitHub Release.
#   git/release-browsered.sh 1.2.3
# Run it from the sbcl-rasp folder.
set -e
cd "$(dirname "$0")/.."

version=$1
if [ -z "$version" ]; then
    echo "Usage: git/release-browsered.sh VERSION   (example: git/release-browsered.sh 1.2.3)"; exit 1
fi
case "$version" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) echo "VERSION must look like 1.2.3"; exit 1 ;;
esac

tag="browsered-v$version"
git tag -a "$tag" -m "Browsered $version"
git push origin "$tag"

echo "Pushed $tag. GitHub Actions will build Linux, macOS and Windows and publish the release."
echo "See https://github.com/FemtoEmacs/sbcl-rasp/actions"
