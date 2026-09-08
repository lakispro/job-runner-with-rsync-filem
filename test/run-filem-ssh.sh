#!/usr/bin/env bash
#
# filem/rsync on its own, launched with plm/ssh across three plain
# containers - no Kubernetes anywhere. Two reasons this exists next to
# run-k8s-tests.sh:
#
#   * it shows filem/rsync is PLM-independent: it moves the files over the
#     HNP-to-prted TCP path, so whatever started the daemons is irrelevant;
#   * it is the fast loop for debugging pre-positioning, since a failure
#     here is never a cluster, an image import or a pod scheduling problem.
#
# Usage:  test/run-filem-ssh.sh [--build] [--keep]
set -uo pipefail

cd "$(dirname "$0")"
# shellcheck source=test/lib.sh
. ./lib.sh

KEEP=0
for arg in "$@"; do
    case $arg in
        --build) BUILD=1 ;;
        --keep)  KEEP=1 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

need docker
COMPOSE=(docker compose -f docker-compose.ssh.yml)

if [ "${BUILD:-0}" = 1 ]; then
    say "building $IMAGE"
    (cd "$REPO_ROOT" && docker build -t "$IMAGE" .) || exit 1
fi

cleanup() {
    if [ "$KEEP" = 0 ]; then
        "${COMPOSE[@]}" down -v >/dev/null 2>&1
    fi
}
trap cleanup EXIT

say "starting head + 2 ssh workers"
IMAGE=$IMAGE "${COMPOSE[@]}" up -d --build || exit 1

# sshd needs a moment, and prte will not retry a refused connection.
for _ in $(seq 1 30); do
    if "${COMPOSE[@]}" exec -T head ssh -o ConnectTimeout=2 worker1 true 2>/dev/null \
       && "${COMPOSE[@]}" exec -T head ssh -o ConnectTimeout=2 worker2 true 2>/dev/null; then
        break
    fi
    sleep 1
done

head() { "${COMPOSE[@]}" exec -T -w /work head "$@"; }

say "sanity: plm/ssh reaches both workers"
out=$(head mpirun --host worker1,worker2 --pernode hostname 2>&1)
check "worker1 ran a process" worker1 "$out"
check "worker2 ran a process" worker2 "$out"

say "filem/rsync: --preload-files \$(pwd) --pernode ls"
out=$(head mpirun --prtemca filem rsync \
        --host worker1,worker2 --preload-files /work --pernode ls 2>&1)
check_count "hello.txt on both workers" 2 "hello.txt" "$out"
check_count "script.sh on both workers" 2 "script.sh" "$out"
check_count "subdir on both workers"    2 "subdir"    "$out"

say "filem/rsync: contents survive the trip"
out=$(head mpirun --prtemca filem rsync \
        --host worker1,worker2 --preload-files /work --pernode cat hello.txt 2>&1)
check_count "hello.txt content on both workers" \
    2 "hello from the launching node" "$out"

say "filem/rsync: an executable stays executable"
out=$(head mpirun --prtemca filem rsync \
        --host worker1,worker2 --preload-files /work --pernode ./script.sh 2>&1)
check_count "the preloaded script ran on both workers" 2 "fixture script ran on" "$out"

say "filem/rsync is the default, without being asked for"
out=$(head mpirun --host worker1,worker2 --preload-files /work --pernode ls 2>&1)
check_count "preloading works with no --prtemca filem at all" 2 "hello.txt" "$out"

say "\"as-is\": ls shows the preloaded entries and nothing else"
out=$(head mpirun --host worker1 --preload-files /work -n 1 ls 2>&1 | tr -s '[:space:]' '\n' | sort -u | grep -v '^$')
if [ "$out" = "$(printf 'hello.txt\nscript.sh\nsubdir')" ]; then
    ok "ls output is exactly the preloaded directory's contents"
else
    bad "ls showed something other than the preloaded contents: $(tr '\n' ' ' <<<"$out")"
fi

# NOTE: the stock filem/raw cannot be asked to deliver --preload-files at
# all in PRRTE 3.0.13 - it transfers the bytes fine and then fails the
# launch with PRTE_ERR_IN_ERRNO, even for a purely local run with no
# --host. That is upstream and nothing to do with this repository, so all
# that is checked here is that adding a component did not break selecting
# the one that was already there.
say "filem/raw is still selectable (we did not break the stock component)"
out=$(head mpirun --prtemca filem raw --host worker1,worker2 --pernode hostname 2>&1)
check "a job still runs under filem/raw" worker1 "$out"

summary
