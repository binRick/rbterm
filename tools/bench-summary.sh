#!/usr/bin/env bash
# tools/bench-summary.sh
#
# Print a side-by-side table of the most recent echo_bench result
# per terminal. Reads bench/echo-*-*.txt; for each terminal the
# most recently modified file wins (so re-running bench-here.sh in
# the same terminal updates that row).

set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO/bench"

# Group result files by terminal slug. Pick the newest per slug.
# Newest-first listing → first hit per slug is what we want.
declare_seen=""
files=()
while IFS= read -r f; do
    files+=("$f")
done < <(ls -t echo-*-*.txt 2>/dev/null || true)

if [[ ${#files[@]} -eq 0 ]]; then
    echo "No bench/echo-*-*.txt files found. Run tools/bench-here.sh in"
    echo "each terminal you want to compare first."
    exit 1
fi

printf "%-12s  %-7s  %-8s  %-8s  %-8s  %-8s  %-8s\n" \
       term n min med mean p99 max
printf "%-12s  %-7s  %-8s  %-8s  %-8s  %-8s  %-8s\n" \
       "------------" ------- -------- -------- -------- -------- --------

for f in "${files[@]}"; do
    # Filename: echo-<slug>-<stamp>.txt
    base="${f%.txt}"
    rest="${base#echo-}"
    slug="${rest%-*}"
    # Slug may itself contain dashes (e.g. apple-terminal); strip
    # only the trailing -YYYYMMDD-HHMMSS.
    slug="$(echo "$rest" | sed -E 's/-[0-9]{8}-[0-9]{6}$//')"
    case " $declare_seen " in
        *" $slug "*) continue ;;
    esac
    declare_seen="$declare_seen $slug"
    # Parse the summary line. echo_bench's first line is:
    #   term=<x> n=N min=A med=B mean=C p99=D max=E sd=F ms
    line="$(head -1 "$f" | tr -s ' ')"
    # Anchor every key on a leading space so a key-suffix like `n=`
    # inside `min=` doesn't get picked up by greedy `.*`.
    n=$(echo "$line"   | sed -nE 's/.* n=([0-9]+).*/\1/p')
    mn=$(echo "$line"  | sed -nE 's/.* min=([0-9.]+).*/\1/p')
    md=$(echo "$line"  | sed -nE 's/.* med=([0-9.]+).*/\1/p')
    me=$(echo "$line"  | sed -nE 's/.* mean=([0-9.]+).*/\1/p')
    p99=$(echo "$line" | sed -nE 's/.* p99=([0-9.]+).*/\1/p')
    mx=$(echo "$line"  | sed -nE 's/.* max=([0-9.]+).*/\1/p')
    printf "%-12s  %-7s  %-8s  %-8s  %-8s  %-8s  %-8s\n" \
           "$slug" "${n:-?}" "${mn:-?}" "${md:-?}" "${me:-?}" "${p99:-?}" "${mx:-?}"
done

echo
echo "(units: ms; lower is better. Most recent file per terminal.)"
