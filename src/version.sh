#!/bin/sh -eu

if [ -e "$1/.git" ] && git --version >/dev/null 2>&1 ; then
    git_origin=$(git -C "$1" config --get remote.origin.url)
    git_hash=$(git -C "$1" describe --always --no-abbrev --broken --dirty)
fi

cat >$2 <<EOF
extern const char git_origin[] = "${git_origin:-UNKNOWN}";
extern const char git_hash[] = "${git_hash:-UNKNOWN}";
EOF
