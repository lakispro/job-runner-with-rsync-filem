#
# SPDX-License-Identifier: BSD-3-Clause
#
# Modeled on src/mca/plm/ssh/Makefile.am in the PRRTE tree this component
# is built against - see that file for the DSO/static split explained.
#

EXTRA_DIST = \
        help-plm-k8s.txt

sources = \
        plm_k8s.h \
        plm_k8s_component.c \
        plm_k8s_module.c \
        plm_k8s_template.h \
        plm_k8s_template.c \
        plm_k8s_default_template.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_prte_plm_k8s_DSO
component_noinst =
component_install = prte_mca_plm_k8s.la
else
component_noinst = libprtemca_plm_k8s.la
component_install =
endif

mcacomponentdir = $(prtelibdir)
mcacomponent_LTLIBRARIES = $(component_install)
prte_mca_plm_k8s_la_SOURCES = $(sources)
prte_mca_plm_k8s_la_LDFLAGS = -module -avoid-version
prte_mca_plm_k8s_la_LIBADD = $(top_builddir)/src/libprrte.la

noinst_LTLIBRARIES = $(component_noinst)
libprtemca_plm_k8s_la_SOURCES = $(sources)
libprtemca_plm_k8s_la_LDFLAGS = -module -avoid-version
