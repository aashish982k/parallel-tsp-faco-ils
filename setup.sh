#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"

faco_revision=05b7c1b8d52b27d121b6138222f986fcbf196863
for command in g++ curl tar; do
    command -v "$command" >/dev/null || { echo "Missing: $command" >&2; exit 1; }
done
mkdir -p deps
if [[ -d deps/faco ]]; then
    [[ -f deps/faco/revision && $(cat deps/faco/revision) == "$faco_revision" ]] || {
        echo "Existing dependency has an unknown revision; use a fresh project directory." >&2
        exit 1
    }
else
    download_directory=$(mktemp -d)
    trap 'rm -rf -- "$download_directory"' EXIT
    curl --fail --location --retry 3 --max-time 120 \
        "https://codeload.github.com/RSkinderowicz/FocusedACO/tar.gz/$faco_revision" \
        -o "$download_directory/faco.tar.gz"
    mkdir "$download_directory/source"
    tar -xzf "$download_directory/faco.tar.gz" --strip-components=1 \
        -C "$download_directory/source"
    printf '%s\n' "$faco_revision" > "$download_directory/source/revision"
    mv -- "$download_directory/source" deps/faco
fi
echo "Pinned FACO dependencies ready. No Python packages are needed."
