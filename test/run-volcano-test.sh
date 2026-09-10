#!/usr/bin/env bash
#
# The anonymous-node mode on a real Volcano Job: "give me N nodes", one
# gang-scheduled object, Volcano picks the nodes, MPI runs across them.
#
# Kept out of run-k8s-tests.sh because it needs Volcano installed; this
# script skips cleanly if it is not. To install it into the k3d cluster:
#
#   kubectl apply -f https://raw.githubusercontent.com/volcano-sh/volcano/release-1.9/installer/volcano-development.yaml
#   kubectl -n volcano-system wait --for=condition=Available deploy --all --timeout=400s
#
# Usage:  test/run-volcano-test.sh [--build]
set -uo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=test/lib.sh
. test/lib.sh

for arg in "$@"; do
    case $arg in
        --build) BUILD=1 ;;
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

if ! kube get crd jobs.batch.volcano.sh >/dev/null 2>&1; then
    say "Volcano is not installed in cluster '$CLUSTER' - skipping"
    echo "   install it with the command in the header of this script"
    exit 0
fi

ensure_launcher || { echo "error: launcher pod never became ready" >&2; exit 1; }

# The launcher's ClusterRole covers batch/v1 Jobs and pods; a Volcano Job
# is a different resource, and both creating and cleaning it up need
# saying so explicitly.
say "granting the launcher rights on Volcano resources"
kube create clusterrole vc-mpi-launcher \
    --resource=jobs.batch.volcano.sh,podgroups.scheduling.volcano.sh \
    --verb=create,get,list,watch,delete,deletecollection,patch,update \
    --dry-run=client -o yaml | kube apply -f - >/dev/null
kube create clusterrolebinding vc-mpi-launcher \
    --clusterrole=vc-mpi-launcher --serviceaccount=default:"$LAUNCHER" \
    --dry-run=client -o yaml | kube apply -f - >/dev/null

mapfile -t NODES < <(agent_nodes)
SELECTOR=prrte.kubepmix.dev/managed-by=plm-k8s
KINDS=jobs.batch.volcano.sh,pod
TMPL=/opt/job-runner/templates/volcano-job.yaml.tmpl
kube delete jobs.batch.volcano.sh -l "$SELECTOR" --ignore-not-found >/dev/null 2>&1

VC="--prtemca plm_k8s_template $TMPL --prtemca plm_k8s_assign_nodes 0 \
    --prtemca plm_k8s_cleanup_kinds $KINDS"

say "real MPI over one gang-scheduled Volcano Job, on anonymous nodes"
out=$(launch "mpirun $VC --host vc-anon-0:3,vc-anon-1:3 --preload-files \$(pwd) -n 5 ./mpi_hello")
check "the collective returned the right answer" "allreduce OK" "$out"
check_count "all 5 ranks reported in" 5 "/5 on " "$out"
if grep -q "vc-anon-" <<<"$out"; then
    bad "a placeholder host name leaked into the run"
    dump "$out"
else
    ok "placeholder names were replaced by the nodes Volcano chose"
fi

say "it really was one Volcano Job, with a gang"
launch "mpirun $VC --host vc-anon-0,vc-anon-1 --pernode sleep 40" >"$WORKDIR/vc.log" 2>&1 &
mpirun_pid=$!
for _ in $(seq 1 90); do
    running=$(kube get pods -l "$SELECTOR" --no-headers 2>/dev/null | grep -c Running)
    [ "$running" = "2" ] && break
    sleep 1
done
vcjobs=$(kube get jobs.batch.volcano.sh -l "$SELECTOR" --no-headers 2>/dev/null | grep -c .)
plainjobs=$(kube get jobs -l "$SELECTOR" --no-headers 2>/dev/null | grep -c .)
minavail=$(kube get jobs.batch.volcano.sh -l "$SELECTOR" \
    -o jsonpath='{.items[0].spec.minAvailable}' 2>/dev/null)
info "volcano jobs: $vcjobs, batch/v1 jobs: $plainjobs, minAvailable: $minavail, running pods: $running"

[ "$vcjobs" = "1" ] && ok "exactly one Volcano Job for the whole DVM" \
    || bad "expected 1 Volcano Job, found $vcjobs"
[ "$plainjobs" = "0" ] && ok "and no stray batch/v1 Job" \
    || bad "expected no batch/v1 Jobs, found $plainjobs"
[ "$minavail" = "2" ] && ok "gang size matches the daemon count" \
    || bad "expected minAvailable=2, got '$minavail'"
wait $mpirun_pid

say "cleanup reaches the custom resource"
# plm_k8s_cleanup_kinds is what makes this work: "kubectl delete job"
# resolves to batch/v1 and would report success while leaving the Volcano
# Job behind. The delete is issued --wait=false, so poll.
for _ in $(seq 1 30); do
    left=$(kube get jobs.batch.volcano.sh,pods -l "$SELECTOR" --no-headers 2>/dev/null | grep -c .)
    [ "$left" = "0" ] && break
    sleep 1
done
[ "$left" = "0" ] && ok "no Volcano Jobs or pods left behind" \
    || { bad "$left object(s) survived shutdown"; kube get jobs.batch.volcano.sh,pods -l "$SELECTOR"; }

summary
