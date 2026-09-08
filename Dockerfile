# syntax=docker/dockerfile:1
#
# One image containing Open MPI 5.0.10 and the two PRRTE components in
# this repository (plm/k8s and filem/rsync), built against exactly the
# OpenPMIx and PRRTE that Open MPI 5.0.10 ships.
#
#   docker build -t job-runner:latest .
#
# ---------------------------------------------------------------------
# Why these versions
# ---------------------------------------------------------------------
# Open MPI bundles OpenPMIx and PRRTE as submodules, and mpirun is
# literally prterun: an Open MPI built against a *different* PRRTE than it
# expects is the usual source of "mpirun works but nothing launches". The
# 5.0.10 tarball's 3rd-party/openpmix and 3rd-party/prrte trees are
# byte-identical to the upstream v5.0.10 and v3.0.13 tags respectively -
# verified by diffing every source file, not by reading release notes - so
# those are the two revisions pinned below, and Open MPI is configured
# --with-pmix/--with-prrte pointing at them rather than building its own
# copies. That is what makes the components in this repo, which are built
# into that same PRRTE, visible to mpirun.
#
# (PRRTE's own VERSION file in the 5.0.10 tarball says 3.0.13 with
# tarball_version=gitclone, which is why the tag has to be confirmed by
# comparing sources rather than trusted from the metadata.)
ARG OPENPMIX_REPO=https://github.com/openpmix/openpmix
ARG OPENPMIX_REVISION=v5.0.10
ARG PRRTE_REPO=https://github.com/openpmix/prrte
ARG PRRTE_REVISION=v3.0.13
ARG OMPI_VERSION=5.0.10
ARG OMPI_SERIES=v5.0

ARG KUBECTL_VERSION=v1.31.0
ARG BASE_IMAGE=ubuntu:24.04

# Extra ./configure flags for Open MPI. Fortran is on by default because
# an "MPI image" without it surprises people; pass
# --build-arg OMPI_CONFIGURE_EXTRA=--disable-mpi-fortran for a faster,
# smaller build.
ARG OMPI_CONFIGURE_EXTRA=

# ---------------------------------------------------------------------
# Stage 1: OpenPMIx
# ---------------------------------------------------------------------
FROM ${BASE_IMAGE} AS pmix-build

RUN apt-get update >/dev/null && apt-get install -y --no-install-recommends \
        git autoconf automake libtool build-essential perl pkg-config \
        flex libevent-dev libhwloc-dev zlib1g-dev ca-certificates \
        python3 >/dev/null \
    && rm -rf /var/lib/apt/lists/*

ARG OPENPMIX_REPO
ARG OPENPMIX_REVISION

RUN git clone --depth 1 --branch ${OPENPMIX_REVISION} ${OPENPMIX_REPO} /openpmix
WORKDIR /openpmix
RUN git submodule update --init --depth 1
RUN ./autogen.pl --quiet
# hwloc and libevent come from the distro - found on the default search
# paths, which is why no --with flag is needed here. PRRTE picks up the
# same ones the same way, and Open MPI is told "external" below rather
# than being allowed to build its own: all three have to agree on them,
# and two libevents in one address space is a bad afternoon.
RUN ./configure --prefix=/opt/pmix >/dev/null
RUN make -j"$(nproc)" >/dev/null && make install >/dev/null

# ---------------------------------------------------------------------
# Stage 2: PRRTE, with this repository's components staged into its tree
# ---------------------------------------------------------------------
FROM ${BASE_IMAGE} AS prrte-build

RUN apt-get update >/dev/null && apt-get install -y --no-install-recommends \
        git autoconf automake libtool build-essential perl pkg-config \
        flex libevent-dev libhwloc-dev zlib1g-dev ca-certificates \
        python3 >/dev/null \
    && rm -rf /var/lib/apt/lists/*

ARG PRRTE_REPO
ARG PRRTE_REVISION

RUN git clone --depth 1 --branch ${PRRTE_REVISION} ${PRRTE_REPO} /prrte
WORKDIR /prrte
RUN git submodule update --init --depth 1

COPY --from=pmix-build /opt/pmix /opt/pmix

# PRRTE discovers MCA components by autogen.pl scanning src/mca/<framework>/
# at build time, so a component has to be in the tree *before* autogen runs
# - there is no supported way to drop one into an already-configured build.
# These two COPYs land right after the clone, so editing a component only
# invalidates Docker's cache from here on: no re-clone, no rebuilding
# OpenPMIx.
COPY plm_k8s/ src/mca/plm/k8s/
COPY filem_rsync/ src/mca/filem/rsync/

RUN ./autogen.pl --quiet
RUN ./configure --prefix=/opt/prte --with-pmix=/opt/pmix >/dev/null
RUN make -j"$(nproc)" >/dev/null && make install >/dev/null

# Fail the build now, rather than at "mpirun" time, if either component
# failed to make it in. Both are built statically into libprrte, so the
# evidence is in prte_info rather than in a .so on disk.
RUN /opt/prte/bin/prte_info | grep -q 'MCA plm: k8s ' \
    && /opt/prte/bin/prte_info | grep -q 'MCA filem: rsync ' \
    && echo "plm/k8s and filem/rsync are present"

# ---------------------------------------------------------------------
# Stage 3: Open MPI against that exact PMIx and PRRTE
# ---------------------------------------------------------------------
FROM ${BASE_IMAGE} AS ompi-build

RUN apt-get update >/dev/null && apt-get install -y --no-install-recommends \
        build-essential gfortran perl pkg-config bzip2 curl ca-certificates \
        libevent-dev libhwloc-dev zlib1g-dev python3 >/dev/null \
    && rm -rf /var/lib/apt/lists/*

ARG OMPI_VERSION
ARG OMPI_SERIES
ARG OMPI_CONFIGURE_EXTRA

COPY --from=pmix-build /opt/pmix /opt/pmix
COPY --from=prrte-build /opt/prte /opt/prte

RUN curl -fsSL -o /tmp/ompi.tar.bz2 \
        "https://download.open-mpi.org/release/open-mpi/${OMPI_SERIES}/openmpi-${OMPI_VERSION}.tar.bz2" \
    && tar -xf /tmp/ompi.tar.bz2 -C /tmp \
    && rm /tmp/ompi.tar.bz2

WORKDIR /tmp/openmpi-${OMPI_VERSION}
RUN ./configure --prefix=/opt/ompi \
        --with-pmix=/opt/pmix \
        --with-prrte=/opt/prte \
        --with-hwloc=external \
        --with-libevent=external \
        --disable-sphinx \
        ${OMPI_CONFIGURE_EXTRA} >/dev/null
RUN make -j"$(nproc)" >/dev/null && make install >/dev/null

# ---------------------------------------------------------------------
# Stage 4: runtime
# ---------------------------------------------------------------------
FROM ${BASE_IMAGE} AS runtime

# rsync is not optional here: filem/rsync shells out to it both as the
# exporting daemon on the launching node and as the client in every
# prted's container, so it has to exist on both sides of a launch - and
# in this image those are the same image.
RUN apt-get update >/dev/null && apt-get install -y --no-install-recommends \
        libevent-2.1-7 libevent-pthreads-2.1-7 libhwloc15 zlib1g \
        rsync openssh-client ca-certificates curl gfortran \
        libgomp1 >/dev/null \
    && rm -rf /var/lib/apt/lists/*

ARG KUBECTL_VERSION
RUN curl -fsSL -o /usr/local/bin/kubectl \
        "https://dl.k8s.io/release/${KUBECTL_VERSION}/bin/linux/$(dpkg --print-architecture)/kubectl" \
    && chmod +x /usr/local/bin/kubectl

COPY --from=pmix-build /opt/pmix /opt/pmix
COPY --from=prrte-build /opt/prte /opt/prte
COPY --from=ompi-build /opt/ompi /opt/ompi

# Both shipped templates, so they can be read, diffed and copied from a
# running container. The single-Indexed-Job one is also installed at the
# path plm/k8s looks in by default - it is identical to the copy compiled
# into the component, so this changes nothing until you edit it (which is
# the point: "sed -i s|job-runner:latest|myregistry/mine:tag|" is the
# common first customization).
COPY templates/ /opt/job-runner/templates/
RUN cp /opt/job-runner/templates/single-job.yaml.tmpl /root/.plm-k8s-template

# Register the libraries with the dynamic linker rather than relying on
# LD_LIBRARY_PATH alone: anything that reaches this image over ssh, or
# through an entrypoint that resets the environment, still finds them.
RUN printf '/opt/ompi/lib\n/opt/prte/lib\n/opt/pmix/lib\n' \
        > /etc/ld.so.conf.d/job-runner.conf \
    && ldconfig

ENV PATH=/opt/ompi/bin:/opt/prte/bin:/opt/pmix/bin:/usr/local/bin:/usr/bin:/bin
ENV LD_LIBRARY_PATH=/opt/ompi/lib:/opt/prte/lib:/opt/pmix/lib
ENV HOME=/root
# Containers here run as root and there is nobody to warn about it.
ENV PRTE_ALLOW_RUN_AS_ROOT=1
ENV PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1
ENV OMPI_ALLOW_RUN_AS_ROOT=1
ENV OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1

WORKDIR /work
CMD ["/bin/bash"]
