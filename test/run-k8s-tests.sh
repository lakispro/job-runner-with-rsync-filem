#!/usr/bin/env bash
#
# The two goals this repository exists for, checked against a real
# (k3d-in-docker) multi-node Kubernetes cluster:
#
#   1. filem/rsync    mpirun --preload-files $(pwd) --pernode ls
#                     shows the launching directory's contents on every node
#   2. plm/k8s        mpirun --host a,b creates ONE Kubernetes object
#                     covering both daemons, not one per node
#
# mpirun runs inside the cluster, in the launcher pod described by
# launcher.yaml - see that file for why it has to.
#
# Usage:  test/run-k8s-tests.sh [--build] [--delete]
#
#   --build    build the image first (docker build -t $IMAGE .)
#   --delete   tear the k3d cluster down afterwards (default: leave it up,
#              so a re-run is fast)
set -uo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=test/lib.sh
. test/lib.sh

DELETE=0
for arg in "$@"; do
    case $arg in
        --build)  BUILD=1 ;;
        --delete) DELETE=1 ;;
        --keep)   DELETE=0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

need docker
need k3d
need kubectl

if [ "${BUILD:-0}" = 1 ]; then
    say "building $IMAGE"
    docker build -t "$IMAGE" . || exit 1
fi

ensure_cluster
ensure_launcher || { echo "error: launcher pod never became ready" >&2; exit 1; }

mapfile -t NODES < <(agent_nodes)
if [ "${#NODES[@]}" -lt 2 ]; then
    echo "error: need at least 2 worker nodes, found ${#NODES[@]}" >&2
    exit 1
fi
HOSTS=$(IFS=,; echo "${NODES[*]}")
info "worker nodes: $HOSTS"

SELECTOR=prrte.kubepmix.dev/managed-by=plm-k8s

# Leftovers from an interrupted previous run would make the
# "exactly one object" assertion meaningless.
kube delete job,pod -l "$SELECTOR" --ignore-not-found >/dev/null 2>&1

###########################################################################
say "GOAL 1: filem/rsync - --preload-files \$(pwd) --pernode ls"
###########################################################################
out=$(launch "mpirun --prtemca plm k8s --prtemca filem rsync \
        --host $HOSTS --preload-files \$(pwd) --pernode ls")
check_count "hello.txt listed on both nodes"  2 "hello.txt" "$out"
check_count "script.sh listed on both nodes"  2 "script.sh" "$out"
check_count "subdir listed on both nodes"     2 "subdir"    "$out"

say "GOAL 1b: 'as-is' - ls shows the preloaded entries and nothing else"
raw=$(launch "mpirun --prtemca plm k8s --host ${NODES[0]} --preload-files \$(pwd) -n 1 ls")
got=$(tr -s '[:space:]' '\n' <<<"$raw" | grep -v '^$' | sort -u | tr '\n' ' ')
if [ "$got" = "hello.txt script.sh subdir " ]; then
    ok "ls output is exactly the launching directory's contents"
else
    bad "ls showed something other than the preloaded contents: $got"
    dump "$raw"
fi

say "GOAL 1c: contents and structure survive the trip"
out=$(launch "mpirun --prtemca plm k8s --host $HOSTS --preload-files \$(pwd) --pernode cat hello.txt")
check_count "file contents intact on both nodes" 2 "hello from the launching node" "$out"

out=$(launch "mpirun --prtemca plm k8s --host $HOSTS --preload-files \$(pwd) --pernode cat subdir/nested.txt")
check_count "nested file arrived on both nodes" 2 "line two" "$out"

say "GOAL 1d: a preloaded executable is still executable"
out=$(launch "mpirun --prtemca plm k8s --host $HOSTS --preload-files \$(pwd) --pernode ./script.sh")
check_count "the preloaded script ran on both nodes" 2 "fixture script ran on" "$out"

###########################################################################
say "GOAL 2: plm/k8s - --host a,b creates ONE templated Kubernetes object"
###########################################################################
# Hold the DVM up long enough to look at the cluster while it is running:
# the objects are deleted on shutdown, so counting them afterwards would
# always find zero.
launch "mpirun --prtemca plm k8s --host $HOSTS --pernode sleep 40" >"$WORKDIR/plm.log" 2>&1 &
mpirun_pid=$!

pods_running=0
for _ in $(seq 1 90); do
    pods_running=$(kube get pods -l "$SELECTOR" --no-headers 2>/dev/null | grep -c Running)
    [ "$pods_running" = "2" ] && break
    sleep 1
done

jobs_found=$(kube get jobs -l "$SELECTOR" --no-headers 2>/dev/null | wc -l)
job_names=$(kube get jobs -l "$SELECTOR" -o name 2>/dev/null | tr '\n' ' ')
pod_nodes=$(kube get pods -l "$SELECTOR" \
    -o jsonpath='{range .items[*]}{.spec.nodeName}{"\n"}{end}' 2>/dev/null | sort -u)

info "jobs: $jobs_found ($job_names)"
info "pods: $pods_running running, on: $(tr '\n' ' ' <<<"$pod_nodes")"

if [ "$jobs_found" = "1" ]; then
    ok "exactly one Kubernetes Job was created for both daemons"
else
    bad "expected exactly 1 Job for the whole DVM, found $jobs_found ($job_names)"
    kube get jobs -l "$SELECTOR" -o wide
fi

if [ "$pods_running" = "2" ]; then
    ok "that one Job produced one daemon pod per node"
else
    bad "expected 2 running daemon pods, found $pods_running"
    kube get pods -l "$SELECTOR" -o wide
fi

if [ "$(wc -l <<<"$pod_nodes")" = "2" ]; then
    ok "the daemon pods landed on two distinct nodes"
else
    bad "expected the daemons on 2 distinct nodes, got: $(tr '\n' ' ' <<<"$pod_nodes")"
fi

wait $mpirun_pid
info "mpirun exited $? - output in $WORKDIR/plm.log"

say "GOAL 2b: the DVM's objects are cleaned up on shutdown"
# The delete is issued with --wait=false, so the API server may still be
# reaping when mpirun exits; give it a moment before calling it a leak.
for _ in $(seq 1 30); do
    leftovers=$(kube get jobs,pods -l "$SELECTOR" --no-headers 2>/dev/null | grep -c .)
    [ "$leftovers" = "0" ] && break
    sleep 1
done
if [ "$leftovers" = "0" ]; then
    ok "no Jobs or pods left behind"
else
    bad "$leftovers object(s) survived shutdown"
    kube get jobs,pods -l "$SELECTOR"
fi

say "GOAL 2c: processes really ran on the worker nodes"
out=$(launch "mpirun --prtemca plm k8s --host $HOSTS --pernode hostname")
for n in "${NODES[@]}"; do
    check_line "a process ran on $n" "$n" "$out"
done

say "GOAL 2d: PRRTE's node table matches where the daemons actually landed"
# The pods are placed by the Kubernetes scheduler, so this is the check
# that the node-to-vpid lookup in the template is doing its job: if a
# daemon took the identity PRRTE meant for a different node, the two nodes
# swap names in PRRTE's table and the DVM corrupts itself. Two processes,
# each reporting a different allocated hostname, is that not having
# happened.
printf '%s\n' "${NODES[@]}" > "$WORKDIR/nodes.txt"
uniq_hosts=$(sort -u <<<"$out" | grep -cFx -f "$WORKDIR/nodes.txt")
if [ "$uniq_hosts" = "2" ]; then
    ok "each daemon reported the node PRRTE had assigned it"
else
    bad "expected 2 distinct allocated hostnames in the output, got $uniq_hosts"
    dump "$out"
fi

say "GOAL 2e: the alternative nodeName-pinned template also launches"
out=$(launch "mpirun --prtemca plm k8s \
        --prtemca plm_k8s_template /opt/job-runner/templates/list-of-jobs.yaml.tmpl \
        --host $HOSTS --pernode hostname")
for n in "${NODES[@]}"; do
    check_line "list-of-jobs template ran a process on $n" "$n" "$out"
done

###########################################################################
say "Both together: the user's commands, verbatim"
###########################################################################
# No --prtemca here: the launcher pod sets PRTE_MCA_plm=k8s in its
# environment (see launcher.yaml), which is how a pod whose job is
# launching into the cluster should be configured. So these are exactly
# the two commands, as typed.
# --host is still needed: it is the allocation. Without it PRRTE has only
# the launcher's own node, and --pernode means one process, on it.
out=$(launch "mpirun --preload-files \$(pwd) --host $HOSTS --pernode ls")
check_count "preloaded content on both nodes, with no --prtemca at all" 2 "hello.txt" "$out"

out=$(launch "mpirun --host $HOSTS hostname")
for n in "${NODES[@]}"; do
    check_line "--host <a,b> alone launched into Kubernetes and ran on $n" "$n" "$out"
done

if [ "$DELETE" = 1 ]; then
    say "deleting k3d cluster '$CLUSTER'"
    k3d cluster delete "$CLUSTER"
fi

summary
