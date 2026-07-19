#!/bin/sh
set -eu

image=$1
platform=$2
repo=$3
make_cmd=$4
shift 4

if uid=$(id -u 2>/dev/null) && gid=$(id -g 2>/dev/null); then
    docker run --rm \
        --platform "$platform" \
        --user "$uid:$gid" \
        -v "$repo":/usr/src/apheleia \
        -w /usr/src/apheleia \
        "$image" "$make_cmd" "$@"
else
    docker run --rm \
        --platform "$platform" \
        -v "$repo":/usr/src/apheleia \
        -w /usr/src/apheleia \
        "$image" "$make_cmd" "$@"
fi
