#!/bin/sh
# Copy your own Halo CE game data to the PS4 over GoldHEN's FTP server.
#
#   scripts/upload-maps.sh <PS4_IP> <local dir> [remote subdir]
#
#   scripts/upload-maps.sh 192.168.1.50 ~/halo/maps          # -> /data/halo/maps
#   scripts/upload-maps.sh 192.168.1.50 ~/halo/sounds sounds # -> /data/halo/sounds
#
# The remote subdirectory defaults to the local directory's name. Enable the
# FTP server in GoldHEN's settings first (port 2121). Environment:
# PS4_FTP_PORT (default 2121), PS4_DATA_ROOT (default /data/halo).
set -eu
if [ $# -lt 2 ]; then
    sed -n '2,12p' "$0" >&2
    exit 1
fi
ip=$1
src=${2%/}
sub=${3:-$(basename "$src")}
port=${PS4_FTP_PORT:-2121}
root=${PS4_DATA_ROOT:-/data/halo}
[ -d "$src" ] || { echo "upload-maps.sh: $src is not a directory" >&2; exit 1; }
command -v curl >/dev/null 2>&1 || { echo "upload-maps.sh: needs curl" >&2; exit 1; }

count=0
failed=0
# find | while would lose the counters in a subshell; use a temporary list
list=$(mktemp)
trap 'rm -f "$list"' EXIT
(cd "$src" && find . -type f | sed 's|^\./||' | sort) > "$list"
total=$(wc -l < "$list" | tr -d ' ')
while IFS= read -r rel; do
    count=$((count + 1))
    url="ftp://$ip:$port$root/$sub/$rel"
    printf '[%s/%s] %s\n' "$count" "$total" "$root/$sub/$rel"
    # GoldHEN's server expects passive mode; --ftp-create-dirs makes /data/halo/...
    if ! curl --silent --show-error --ftp-pasv --ftp-create-dirs --connect-timeout 10 \
            -T "$src/$rel" "$url"; then
        failed=$((failed + 1))
    fi
done < "$list"
echo "uploaded $((count - failed))/$total files to $ip:$root/$sub"
[ "$failed" = 0 ]
