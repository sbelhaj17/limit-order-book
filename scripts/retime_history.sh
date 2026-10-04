#!/bin/bash
# Re-time the replay at each commit that changed its speed, on the full day,
# with one method for all of them.
#
#   scripts/retime_history.sh [itch file]
#
# The commits were first timed while I wrote them, on a 1.5 GB slice and by
# wall clock (the numbers in their commit messages). This replays the whole
# day at each one, under /usr/bin/time -l, which gives user CPU, system CPU and
# wall time the same way whatever the tool itself printed back then. Each
# commit is built in a temporary worktree, Release, using the GoogleTest and
# Benchmark sources already in build/_deps so nothing is downloaded. The file
# is read once first so every run starts with it in the page cache.
#
# Output: results/retime.txt (one line per run, then the medians) and
# results/replay_<commit>.txt (what the tool printed on its last run).
set -euo pipefail
cd "$(dirname "$0")/.."
DATA=${1:-data/12302019.NASDAQ_ITCH50}
RUNS=${RUNS:-3}
COMMITS="096b370 cde3aa3 1f69eb8 09ee4e0 b744639 de7916a 6624666 HEAD"
DEPS=$PWD/build/_deps
mkdir -p results
OUT=results/retime.txt

echo "warming the page cache with $DATA"
cat "$DATA" > /dev/null

{
    echo "# full-day replay of $DATA, $RUNS runs per commit, file in the page cache"
    echo "# $(sysctl -n machdep.cpu.brand_string), $(sw_vers -productName) $(sw_vers -productVersion), $(date '+%Y-%m-%d %H:%M')"
    echo "# commit   run  user_s  sys_s  wall_s"
} > "$OUT"

for c in $COMMITS; do
    sha=$(git rev-parse --short "$c")
    wt=$(mktemp -d)/lob
    git worktree add --detach "$wt" "$c" > /dev/null 2>&1
    cmake -S "$wt" -B "$wt/build" -DCMAKE_BUILD_TYPE=Release -DLOB_BENCH=OFF \
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
        -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$DEPS/googletest-src" \
        -DFETCHCONTENT_SOURCE_DIR_BENCHMARK="$DEPS/benchmark-src" > /dev/null
    cmake --build "$wt/build" --target itch_replay -j 8 > /dev/null
    for run in $(seq "$RUNS"); do
        /usr/bin/time -l "$wt/build/itch_replay" "$DATA" > "results/replay_$sha.txt" 2> "$wt/time.txt"
        user=$(awk '/ user /{print $3}' "$wt/time.txt")
        sys=$(awk '/ user /{print $5}' "$wt/time.txt")
        wall=$(awk '/ real /{print $1}' "$wt/time.txt")
        printf '%-9s  %3d  %6s  %5s  %6s\n' "$sha" "$run" "$user" "$sys" "$wall" | tee -a "$OUT"
    done
    git worktree remove --force "$wt"
done

# Medians per commit, and user CPU per message using the count HEAD reports.
msgs=$(grep -o '[0-9][0-9,]* messages' "results/replay_$(git rev-parse --short HEAD).txt" | head -1 | tr -d ', messages')
{
    echo
    echo "# medians; ns/msg = median user CPU / $msgs messages"
    echo "# commit   user_s  sys_s  wall_s  user_ns_per_msg"
    awk -v n="$msgs" '!/^#/ && NF == 5 {
        u[$1] = u[$1] " " $3; s[$1] = s[$1] " " $4; w[$1] = w[$1] " " $5; if (!($1 in seen)) { order[++k] = $1; seen[$1] = 1 }
    }
    function med(str,   a, m, i, j, t) {
        m = split(str, a, " ")
        for (i = 1; i <= m; i++) for (j = i + 1; j <= m; j++) if (a[j] + 0 < a[i] + 0) { t = a[i]; a[i] = a[j]; a[j] = t }
        return a[int((m + 1) / 2)]
    }
    END { for (i = 1; i <= k; i++) { c = order[i]; mu = med(u[c]); printf "%-9s  %6s  %5s  %6s  %6.1f\n", c, mu, med(s[c]), med(w[c]), mu * 1e9 / n } }' "$OUT"
} | tee -a "$OUT"
