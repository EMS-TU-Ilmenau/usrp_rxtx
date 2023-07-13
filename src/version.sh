#!/bin/sh -eu

if git --version >/dev/null 2>&1 && git -C "$1" status >/dev/null 2>&1 && git -C "$1" describe >/dev/null 2>&1 ; then
    echo "extern const char git_origin[] = \"$(git -C "$1" config --get remote.origin.url)\";" >$2
    echo "extern const char git_hash[] = \"$(git -C "$1" describe --always --long --broken --dirty)\";" >>$2
else
    echo "extern const char git_origin[] = \"UNKNOWN\";" >$2
    echo "extern const char git_hash[] = \"UNKNOWN\";" >>$2
fi
