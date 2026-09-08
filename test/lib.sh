# shellcheck shell=bash
# Shared helpers for the test scripts in this directory.

set -o pipefail

IMAGE=${IMAGE:-job-runner:latest}
CLUSTER=${CLUSTER:-jr}
REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORKDIR=${WORKDIR:-$(mktemp -d)}
KUBECFG="$WORKDIR/kubeconfig"
LAUNCHER=mpi-launcher

PASS=0
FAIL=0

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
info() { printf '   %s\n' "$*"; }

ok()   { PASS=$((PASS + 1)); printf '\033[32m   PASS\033[0m %s\n' "$*"; }
bad()  { FAIL=$((FAIL + 1)); printf '\033[31m   FAIL\033[0m %s\n' "$*"; }

dump() { printf '        --- output ---\n'; sed 's/^/        /' <<<"$1"; printf '        --------------\n'; }

# check <description> <expected-substring> <actual-output>
check() {
    local desc=$1 needle=$2 haystack=$3
    if grep -qF -- "$needle" <<<"$haystack"; then
        ok "$desc"
    else
        bad "$desc (expected to find \"$needle\")"
        dump "$haystack"
    fi
}

# check_line <description> <expected-whole-line> <actual-output>
# Substring matching is too loose where an error message can quote the
# thing we are looking for - a node name, say - back at us.
check_line() {
    local desc=$1 needle=$2 haystack=$3
    if grep -qx -- "$needle" <<<"$haystack"; then
        ok "$desc"
    else
        bad "$desc (expected a line reading exactly \"$needle\")"
        dump "$haystack"
    fi
}

# check_count <description> <expected-count> <substring> <actual-output>
check_count() {
    local desc=$1 want=$2 needle=$3 haystack=$4 got
    got=$(grep -cF -- "$needle" <<<"$haystack" || true)
    if [ "$got" = "$want" ]; then
        ok "$desc"
    else
        bad "$desc (expected $want occurrences of \"$needle\", got $got)"
        dump "$haystack"
    fi
}

summary() {
    printf '\n\033[1m%d passed, %d failed\033[0m\n' "$PASS" "$FAIL"
    [ "$FAIL" -eq 0 ]
}

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "error: $1 is required but not on PATH" >&2
        exit 1
    }
}

kube() { KUBECONFIG=$KUBECFG kubectl "$@"; }

# Bring up (or reuse) the k3d cluster the k8s tests run against, and make
# sure the image under test is loaded into every node's containerd -
# nothing pulls job-runner:latest from a registry, so an un-imported image
# is a mysterious ImagePullBackOff rather than a test failure.
ensure_cluster() {
    local agents=${AGENTS:-2}
    if k3d cluster list "$CLUSTER" >/dev/null 2>&1; then
        info "reusing k3d cluster '$CLUSTER'"
    else
        say "creating k3d cluster '$CLUSTER' with $agents agents"
        k3d cluster create "$CLUSTER" --agents "$agents" --no-lb \
            --k3s-arg "--disable=traefik,servicelb,metrics-server@server:0"
    fi

    say "importing $IMAGE into the cluster"
    k3d image import -c "$CLUSTER" "$IMAGE"

    k3d kubeconfig get "$CLUSTER" > "$KUBECFG"
    chmod 600 "$KUBECFG"
}

# (Re)create the launcher pod and put the fixtures in its working
# directory. It is recreated rather than reused so that a re-run always
# picks up a freshly imported image.
ensure_launcher() {
    say "starting the launcher pod"
    kube delete pod "$LAUNCHER" --ignore-not-found --wait >/dev/null
    kube apply -f "$REPO_ROOT/test/launcher.yaml" >/dev/null
    kube wait --for=condition=Ready "pod/$LAUNCHER" --timeout=180s >/dev/null || return 1
    kube cp "$REPO_ROOT/test/fixtures/." "$LAUNCHER:/work/"
    info "fixtures in /work: $(kube exec "$LAUNCHER" -- ls /work | tr '\n' ' ')"
}

# Run a command inside the launcher, from /work. See launcher.yaml for why
# mpirun has to run in the cluster rather than beside it.
launch() {
    kube exec "$LAUNCHER" -- bash -lc "cd /work && $*" 2>&1
}

# The names of the worker nodes, which are what --host wants. The launcher
# sits on the control-plane node, which is deliberately not among them.
agent_nodes() {
    kube get nodes -l '!node-role.kubernetes.io/control-plane' \
        -o jsonpath='{range .items[*]}{.metadata.name}{"\n"}{end}'
}
