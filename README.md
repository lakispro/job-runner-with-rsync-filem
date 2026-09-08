# job-runner-with-rsync-filem

Open MPI 5.0.10 in a container, plus two PRRTE components that make
`mpirun` usable as a Kubernetes job runner:

- **`plm/k8s`** launches the PRRTE daemons by applying **one** templated
  Kubernetes manifest for the whole DVM, rather than one `Job` per node;
- **`filem/rsync`** pre-positions `--preload-files` with rsync, so
  `mpirun --preload-files $(pwd) --pernode ls` shows you your working
  directory on every worker.

```sh
docker build -t job-runner:latest .

# from a pod running this image, in the cluster (see "Where mpirun runs")
mpirun --prtemca plm k8s --prtemca filem rsync \
       --host node-a,node-b --preload-files "$(pwd)" --pernode ls
hello.txt  script.sh  subdir
hello.txt  script.sh  subdir
```

The `plm/k8s` component is a rework of the one-Job-per-daemon component in
`prrte-k8s-plm`; the differences are described under
[plm/k8s](#plmk8s--one-templated-object-for-the-whole-dvm) below.
`filem/rsync` is new.

## Contents

```
Dockerfile                  Open MPI 5.0.10 + OpenPMIx 5.0.10 + PRRTE 3.0.13
                            + both components, in one image
plm_k8s/                    the PLM component (staged into a PRRTE tree at
                              src/mca/plm/k8s/ - see the Dockerfile)
filem_rsync/                the FILEM component (likewise, src/mca/filem/rsync/)
templates/
  single-job.yaml.tmpl      default: one Job covering every node
  list-of-jobs.yaml.tmpl    alternative: nodeName-pinned Jobs in one document
contrib/
  embed-default-template.py regenerates the built-in copy of the default
test/                       the k3d and ssh test rigs (see Testing)
```

## Versions, and why exactly these

`mpirun` *is* `prterun`, and Open MPI talks to PRRTE and OpenPMIx across
internal interfaces that are only guaranteed to line up within one
release. Building Open MPI against a PRRTE it did not ship with is the
usual root cause of "mpirun starts and then nothing happens", so the whole
image is pinned to what Open MPI 5.0.10 itself bundles:

| Component  | Version   | How it was established |
| ---------- | --------- | ---------------------- |
| Open MPI   | `5.0.10`  | the release tarball |
| OpenPMIx   | `v5.0.10` | `3rd-party/openpmix/VERSION` says `repo_rev=v5.0.10`, and every source file under `src/` is byte-identical to the upstream `v5.0.10` tag |
| PRRTE      | `v3.0.13` | `3rd-party/prrte/VERSION` says `3.0.13` but `repo_rev` is *empty* and `tarball_version=gitclone`, so the tag was confirmed the only reliable way: diffing `src/` against each candidate tag. Against `v3.0.13` there are zero content differences in any common file (the only entries that differ are generated files like `prte_frameworks.c` and `*_lex.c`, present in the tarball but not in git, and files git has that the tarball's dist excludes). `v3.0.12` differs in 79 sources and `v3.0.14` in 14, so it is `v3.0.13` and nothing else. |

The Dockerfile therefore builds OpenPMIx `v5.0.10` and PRRTE `v3.0.13`
from source, stages both components into the PRRTE tree, and configures
Open MPI `--with-pmix=/opt/pmix --with-prrte=/opt/prte` against those exact
builds instead of letting it compile its own copies. hwloc, libevent and
zlib come from the distro for all three, since two libevents in one
address space is its own category of bug.

The image checks itself: the PRRTE stage fails the build if `prte_info`
does not report both components.

## Where `mpirun` runs

**In the cluster, as a pod** — not beside it. This is not a stylistic
preference. The HNP is not a client that pokes the API and walks away: it
is one end of a live out-of-band TCP connection that every `prted` has to
dial back to before the DVM is up, and with `filem/rsync` it also runs the
rsync daemon those same daemons pull from. It needs an address the daemon
pods can reach and ports they can open.

`test/launcher.yaml` is a working example: a `ServiceAccount` with just
the RBAC `plm/k8s` uses (create/delete `jobs`, delete `pods`, read
`nodes`), `hostNetwork: true` so the HNP sits in the same flat address
space the daemon pods land in, and a `nodeSelector` pinning it to a node
that is *not* in the allocation, so the HNP's hostname can never collide
with a daemon's. It also sets `PRTE_MCA_plm=k8s` in the pod environment,
which is what lets `mpirun --host a,b ...` work as typed rather than
needing `--prtemca plm k8s` every time.

Credentials come from wherever `kubectl` would find them — the component
shells out to `kubectl` with no configuration of its own, so a pod's
service account, a mounted `KUBECONFIG`, or `~/.kube/config` all work
unchanged. There is no namespace parameter: put `namespace:` in the
template's `metadata:`, or point `plm_k8s_kubectl_args` at
`--context`/`--kubeconfig`.

## `plm/k8s` — one templated object for the whole DVM

### What changed from one-Job-per-daemon

The ancestor component rendered its template once per daemon and ran one
`kubectl apply` per daemon, so a four-node DVM meant four `Job` objects
and four `kubectl` children. This one renders **once for the whole
launch, with the entire node list in scope**, and applies that single
document with a single `kubectl apply`.

That changes the component's shape in three places:

- the template engine grew a repetition construct (`{{#nodes}}` /
  `{% for node in nodes %}`), because a template that has to describe
  every node needs a way to loop over them;
- the launch is now one child process tracked by one dummy `prte_proc_t`,
  the way `plm/slurm` tracks its single `srun`, instead of a throttled
  per-daemon launch list modelled on `plm/ssh`. `plm_k8s_num_concurrent`
  is gone with it - there is nothing left to run concurrently;
- `ess_base_vpid` moved out of the shared env block, because a single pod
  template cannot carry a per-pod value. See below.

### The default template: a single Job, and how it stays correct

`templates/single-job.yaml.tmpl` renders one `batch/v1` `Job` with
`completions`/`parallelism` equal to the daemon count. One pod template,
N pods, one object. Two things fall out of "one pod template", and the
second one is the whole design:

**Each daemon needs its own vpid, and the shared `env:` block cannot carry
one.** So `{{env}}` at global scope is deliberately *everything except*
`ess_base_vpid`, and each pod works its own out.

**Kubernetes cannot bind replica *i* of a Job to node *i*.** This is not a
detail to wave away. PRRTE's allocation has already decided that daemon 1
is on node A and daemon 2 on node B, and it maps application processes
onto that decision before any pod exists. If the pods come up the other
way round, every daemon reports a hostname PRRTE assigned to a *different*
node — and PRRTE's response to "the daemon reports a different hostname
than the allocation said" is to rename the node and keep the old name as
an alias, which is correct for a hostfile that named a node by IP and
catastrophic for a permutation: two node entries swap names and the DVM
corrupts itself. (Observed as `Fatal glibc error: pthread_mutex_lock.c:450
... assertion failed: e != ESRCH || !robust` from `mpirun`, which is not a
diagnosis anyone would enjoy arriving at from scratch.)

So the template does not try to control placement — it only *constrains*
it, and then makes the daemon's identity a function of where it actually
landed:

```yaml
env:
  - name: PRTE_K8S_NODE                  # the node this pod is really on
    valueFrom:
      fieldRef: { fieldPath: spec.nodeName }
  - name: PRTE_K8S_NODE_VPIDS            # "nodeA:1,nodeB:2" from {{node_vpids}}
    value: "{{node_vpids}}"
```

and the container command looks itself up in that map before `exec prted`,
failing loudly if its node is not in it. `nodeAffinity` keeps the pods on
the allocated nodes and `podAntiAffinity` keeps one daemon per node, so
whatever order the scheduler picks, every daemon takes the vpid PRRTE
assigned to the node it is standing on and PRRTE's map still describes
reality. That is why `plm_k8s_assign_nodes` defaults to **true**.

(That the rest of the daemon's configuration can live in `env:` at all is
because PRRTE's MCA system treats `--prtemca X Y` and `PRTE_MCA_X=Y` as
interchangeable, which is what lets the container command end in a bare
`exec prted`.)

### The other shipped template: pinned per-node Jobs

`templates/list-of-jobs.yaml.tmpl` renders a `kind: List` whose items are
one `Job` per node, each pinned with `nodeName` and each carrying a
complete `{{env}}` including `ess_base_vpid` - so its container command is
a bare `prted` with no index arithmetic:

```sh
mpirun --prtemca plm k8s \
       --prtemca plm_k8s_template /opt/job-runner/templates/list-of-jobs.yaml.tmpl \
       --host node-a,node-b ...
```

`kind: List` is a client-side construct: it is still one document and one
`kubectl apply`, but the API server ends up with N `Job` objects. If you
want one object *and* exact placement, template a CRD that supports
per-replica placement (JobSet, LeaderWorkerSet, a Volcano `PodGroup`) -
the node list is in scope either way, and that is the point of doing the
templating at all.

### Template language

Not Jinja, not Go templates: a small Mustache-shaped substituter, because
PRRTE has no dependency on either and vendoring one for this would be a
poor trade. Anything it does not recognize passes through untouched, so if
you need real control flow, pre-render with a real engine and point
`plm_k8s_template` at the result.

Scalars, in any of `{{x}}`, `{{ x }}`, `{{.x}}`, `{{ .x }}`:

| Tag              | Scope  | Value |
| ---------------- | ------ | ----- |
| `{{name}}`       | both   | sanitized base name shared by this DVM (DNS-1123 safe, ≤50 chars) |
| `{{numnodes}}`   | both   | how many daemons this launch starts |
| `{{vpid_start}}` | both   | the vpid of the first of them |
| `{{nodes_csv}}`  | both   | every node name, comma separated |
| `{{node_vpids}}` | both   | `node:vpid,node:vpid,…` — the allocation's node-to-daemon assignment, in a form a container entrypoint can look itself up in |
| `{{env}}`        | global | the env common to all daemons — everything *except* `ess_base_vpid` |
| `{{env}}`        | node   | this daemon's *complete* env, `ess_base_vpid` included |
| `{{node}}`       | node   | the node PRRTE's allocation picked for this daemon |
| `{{rank}}`       | node   | this daemon's vpid |
| `{{index}}`      | node   | 0-based position within the section |
| `{{slots}}`      | node   | slots the allocation gave this node |

Sections repeat their body once per node, in either spelling, and do not
nest:

```
{{#nodes}} ... {{/nodes}}
{% for node in nodes %} ... {% endfor %}
```

A section tag alone on its line takes the whole line with it (the usual
Mustache standalone-tag rule), so your YAML keeps its indentation. A
multi-line value — `{{env}}` — is re-indented to the column its tag sat
at, provided nothing but whitespace precedes it on the line.

### Cleanup

Not a toggle: it always runs. On shutdown the component issues one
`kubectl delete job,pod -l prrte.kubepmix.dev/managed-by=plm-k8s,prrte.kubepmix.dev/dvm=<name>`,
which is why both shipped templates stamp those labels on everything and
why cleanup works unchanged whether the template produced one object or
twenty. **If you write a template, keep those two labels**, or add your
object's kind to `PRTE_PLM_K8S_CLEANUP_KINDS` in `plm_k8s_module.c` — a
template that drops them leaves cleanup finding nothing, silently.

### MCA parameters

| Parameter                         | Default               | Meaning |
| --------------------------------- | --------------------- | ------- |
| `plm_k8s_template`                | `~/.plm-k8s-template` | Manifest template path (leading `~/` expanded against `$HOME` at load time). Missing file → the built-in copy of `single-job.yaml.tmpl` |
| `plm_k8s_kubectl`                 | `kubectl`             | Binary to look up in `PATH` |
| `plm_k8s_kubectl_args`            | unset                 | Extra args on every kubectl call, e.g. `--context mycluster` |
| `plm_k8s_name_prefix`             | unset                 | Overrides `{{name}}`'s source (default: this DVM's nspace) |
| `plm_k8s_workdir`                 | `$TMPDIR` or `/tmp`   | Where the rendered manifest is staged |
| `plm_k8s_assign_nodes`            | `true`                | Whether the template honours the allocation's node-to-daemon assignment — both shipped ones do. Only set `false` for a template that leaves daemon identity to the scheduler |
| `plm_k8s_apply_timeout`           | `120`                 | Seconds before `kubectl` is given up on (enforced with `alarm(2)`, see below) |
| `plm_k8s_pass_environ_mca_params` | `false`               | Forward `PRTE_MCA_*`/`PMIX_MCA_*` found in our own env |

Selection priority is fixed at 5 — below `plm/ssh`'s 10, so a machine that
has both `ssh` and `kubectl` is not launched into Kubernetes by accident.
Ask for it explicitly with `--prtemca plm k8s`.

### Two things kubectl taught us

`kubectl --request-timeout=<non-zero>` **silently stops kubectl falling
back to in-cluster configuration** (checked on v1.31.0: every other global
flag is fine, and so is `--request-timeout=0`). A launcher running as a
pod then tries `http://localhost:8080` and fails with "connection
refused" despite a perfectly good service account. So
`plm_k8s_apply_timeout` is enforced with `alarm(2)` in the forked child
instead — kubectl-version independent, and nothing a future kubectl
release can reinterpret.

kubectl's **stdout is discarded** on every call this component makes.
"job.batch/prterun-… created" landing in the middle of
`mpirun --pernode ls` output is noise the user cannot filter, and the
cleanup prints its list of deleted objects twice (it runs in
`terminate_orteds` and again in `finalize`, idempotent by design). stderr
is always kept. The object name is available at
`--prtemca plm_base_verbose 1`, where output belongs.

## `filem/rsync` — pre-position with rsync

### Why not `filem/raw`

The stock component reads each preloaded file on the HNP and xcasts it to
every daemon in 4 KiB chunks over the PRRTE out-of-band. It works, but it
is one transfer per *file*, it re-sends bytes that are already on the far
side, and what it delivers lands flattened in the session directory.
"Ship me this whole working directory" is not what it was built for.

`filem/rsync` keeps the same framework contract and the same
HNP-xcasts / daemons-ack shape, and moves the bytes with rsync — which
brings directory trees, permissions, deltas and compression along for
free.

It is also worth recording that in PRRTE 3.0.13 `filem/raw` **cannot
deliver `--preload-files` at all**: it transfers the bytes fine and then
fails the launch with `PRTE_ERR_IN_ERRNO`, even for a purely local run
with no `--host`. That is upstream and not something this repository
changed — `test/run-filem-ssh.sh` checks only that selecting `filem/raw`
still runs a job, since asserting the broken behaviour would be
asserting a bug.

### How the bytes get there

The transport is a short-lived **rsync daemon on the HNP**, not
`rsync -e ssh`: the entire point of running under `plm/k8s` is that there
is no ssh to where the daemons are. But every `prted` can already reach
the HNP over TCP — that is how it phoned home — so:

1. **HNP, `preposition_files()`** — collect every `--preload-files` path
   (and the binary, if `--preload-binary`), hard-link them all into one
   staging tree with `rsync --link-dest`, start `rsync --daemon` on an
   ephemeral port exporting that tree read-only, and xcast where to pull
   from.
2. **Each `prted`** — pull the tree into a per-job stash under its session
   directory and ack. The HNP's addresses (taken from the HNP URI the
   daemon was launched with) are tried in turn, since nothing knows in
   advance which of them is routable from a given node.
3. **HNP** — once every daemon has acked, the launch proceeds.
4. **Each `prted`, `link_local_files()`** — run the job's processes *in*
   that stash. It is already per-job and per-node and lives under the
   session directory PRRTE cleans up, and running in it rather than
   copying it elsewhere is what makes a bare `ls` print exactly what was
   preloaded and nothing else — the job session directory has PRRTE's own
   per-rank subdirectories in it.

   Setting that working directory is not done with `PRTE_APP_SSNDIR_CWD`,
   the attribute that exists for it, because that attribute is unusable on
   a remote daemon in PRRTE 3.0.13: `setup_path()` resolves it by
   dereferencing `app->job`, which is only ever assigned on the HNP, so
   setting it segfaults every non-HNP `prted`. (Reproducible in stock
   PRRTE with `mpirun --host <remote> --preload-binary …` and nothing from
   this repository involved.) `filem/rsync` overwrites `app->cwd` and
   `chdir()`s in `link_local_files()` instead — which `odls` calls after
   `setup_path()` has run and before it resolves a relative executable or
   dispatches the child, so both `./script.sh` and `--preload-binary`'s
   rewritten `./<name>` land correctly.

### What `--preload-files $(pwd)` actually does

The staging step in (1) is where the semantics are decided, once, rather
than N times on the far side:

- a **directory** contributes its *contents* to the root of the tree — so
  `--preload-files $(pwd)` reproduces your working directory rather than
  nesting it one level deep;
- a **file** contributes itself, by basename.

And because `filem_rsync_ssndir_cwd` defaults to `true`, the job's
processes are run in the directory those files landed in. Which is what
makes this work:

```console
$ ls
hello.txt  script.sh  subdir
$ mpirun --prtemca filem rsync --host worker1,worker2 \
         --preload-files "$(pwd)" --pernode ls
hello.txt
script.sh
subdir
hello.txt
script.sh
subdir
```

`cat subdir/nested.txt` and `./script.sh` work the same way.

### Blocking, on purpose

Both rsync invocations block their caller. On the HNP that is a local
hard-link pass. On a `prted` it is the one thing that daemon has to do
before the job can start, and rsync's own `--timeout`/`--contimeout` bound
it — so every daemon acks, with a failure status if it must, rather than
leaving the launch hanging. That property is worth more here than
concurrency, and it is why this is not written as the non-blocking state
machine that `filem/raw`'s chunked transfers have to be.

### MCA parameters

| Parameter                 | Default | Meaning |
| ------------------------- | ------- | ------- |
| `filem_rsync_rsync`       | `rsync` | The binary, looked up in `PATH`. Needed on the HNP *and* in the daemons' image |
| `filem_rsync_port`        | `0`     | Port for the HNP's rsync daemon; 0 asks the OS for a free one. Pin it if a firewall needs a hole |
| `filem_rsync_args`        | unset   | Extra args on every rsync *client* call, e.g. `--exclude=.git` |
| `filem_rsync_timeout`     | `300`   | Seconds of I/O inactivity before a transfer is abandoned |
| `filem_rsync_set_workdir` | `true`  | Run the job where the files landed, so bare `ls` shows them. Ignored for `--preload-binary`, whose rewritten `./<name>` only resolves from there |
| `filem_rsync_priority`    | `5`     | Beats `filem/raw`'s 0, so rsync is the default wherever it exists |

Because the priority default beats `raw`, this image uses rsync for
`--preload-files` without being asked. `--prtemca filem raw` restores the
stock behaviour.

### Security note

The rsync daemon is unauthenticated, `read only`, exports exactly the
staging tree, listens on an ephemeral port, and lives only as long as the
DVM. That is appropriate for a cluster-internal network and is *not*
appropriate on an interface reachable by anyone you would not hand the
contents of `$(pwd)` to. Pin `filem_rsync_port` and firewall it if that
matters to you.

## Testing

Everything is tested in containers; nothing is built or run on the host.

```sh
test/run-k8s-tests.sh --build     # both goals, against a k3d cluster
test/run-filem-ssh.sh --build     # filem/rsync alone, over plm/ssh
```

`run-k8s-tests.sh` creates a k3d cluster (`--agents 2`), imports the image
into it, applies `test/launcher.yaml`, copies `test/fixtures/` into the
launcher's working directory, and runs `mpirun` inside that pod. It
asserts:

- `--preload-files $(pwd) --pernode ls` lists the fixture files on both
  workers; that `ls` output is **exactly** the launching directory's
  contents and nothing else; that `cat` returns them intact, including
  through a subdirectory; and that a preloaded script is still executable;
- `--host a,b` produces **exactly one** `Job`, which produces exactly two
  running daemon pods, on two distinct nodes, and that those objects are
  gone after the DVM shuts down;
- that each daemon reported the node PRRTE had assigned it — the check
  that the node-to-vpid lookup is doing its job, since the two nodes swap
  names in PRRTE's table if it is not;
- that the alternative `nodeName`-pinned template also launches;
- and finally the two commands as typed, with no `--prtemca` at all,
  relying on the launcher pod's `PRTE_MCA_plm=k8s`.

`run-filem-ssh.sh` brings up three plain containers with sshd and runs the
same pre-positioning checks under `plm/ssh`, which is both the fast
debugging loop and the evidence that `filem/rsync` does not depend on
which PLM started the daemons. It also checks that selecting `filem/raw`
still runs a job, since adding a component should not break the one
already there.

## Building

```sh
docker build -t job-runner:latest .
```

Useful build arguments: `OPENPMIX_REVISION`, `PRRTE_REVISION`,
`OMPI_VERSION`/`OMPI_SERIES` (keep them consistent — see the version table
above), `KUBECTL_VERSION`, `BASE_IMAGE`, and
`OMPI_CONFIGURE_EXTRA=--disable-mpi-fortran` for a faster, smaller image.

Both components are copied into the PRRTE tree immediately after the
clone, so editing one invalidates Docker's cache from there on: no
re-clone, no rebuilding OpenPMIx.

To build a component outside Docker, against an existing PRRTE checkout:

```sh
cp -r plm_k8s     /path/to/prrte/src/mca/plm/k8s
cp -r filem_rsync /path/to/prrte/src/mca/filem/rsync
cd /path/to/prrte && ./autogen.pl && ./configure --with-pmix=/opt/pmix && make install
```

`autogen.pl` has to run *after* the copy: PRRTE discovers MCA components
by scanning `src/mca/<framework>/` at that step, and there is no supported
way to load one into a build that did not know about it.

The default template exists twice — as
`templates/single-job.yaml.tmpl` and as a C string compiled into the
component for the case where no template file is present. The C copy is
generated, not maintained:

```sh
./contrib/embed-default-template.py    # after editing the .tmpl
```

## Known limitations

- **`plm/k8s` does not tree-spawn.** The HNP applies the manifest itself
  (`remote_spawn` is unimplemented), same as every non-ssh PLM component.
- **Bare daemon flags are dropped.** The daemon command line is converted
  to `PRTE_MCA_*`/`PMIX_MCA_*` environment assignments, and a flag with no
  env-var equivalent (`--debug-daemons`, `--leave-session-attached`) has
  nowhere to go. It is logged at `plm_base_verbose 1` and skipped. That
  also means daemon output is not visible in `kubectl logs` by default:
  `prted` detaches and redirects to `/dev/null` unless one of those flags
  is on *its* command line, so to see it, copy the template and add the
  flag to the container's `exec prted`.
- **Sections do not nest**, and the template vocabulary is fixed. See
  "Template language".
- **A template that leaves daemon identity to the scheduler will corrupt
  the DVM** if the pods land in a different order than the allocation
  assumed. Use one of the two shipped approaches (node-to-vpid lookup, or
  `nodeName` pinning); see "The default template".
- **`filem/rsync` pre-positions per job, not per DVM.** A second `prun`
  into the same DVM re-runs the whole exchange; nothing is cached between
  jobs beyond the rsync daemon itself staying up.
- **The port race.** Asking the OS for a free port and then handing it to
  a child leaves a window in which something else could take it. Losing
  that race fails the launch with rsync's own "address already in use";
  set `filem_rsync_port` to take the choice into your own hands.
- **Both rsync calls block their caller** (see "Blocking, on purpose").
  On a daemon that is bounded by rsync's own `--timeout`/`--contimeout`;
  on the HNP it is a local hard-link pass over the preloaded tree, which
  is metadata-only but not free for a very large one.
