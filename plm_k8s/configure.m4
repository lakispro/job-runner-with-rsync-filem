# -*- shell-script -*-
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Modeled on src/mca/plm/ssh/configure.m4 - like ssh, this component only
# needs fork()/exec() at build time; the actual "kubectl" binary is
# looked up in PATH at runtime by the component's query function, not at
# configure time.

# MCA_prte_plm_k8s_CONFIG([action-if-found], [action-if-not-found])
# -------------------------------------------------------------------
AC_DEFUN([MCA_prte_plm_k8s_CONFIG],[
    AC_CONFIG_FILES([src/mca/plm/k8s/Makefile])

    AC_CHECK_FUNC([fork], [plm_k8s_happy="yes"], [plm_k8s_happy="no"])

    PRTE_SUMMARY_ADD([Resource Managers], [k8s], [], [$plm_k8s_happy])
    AS_IF([test "$plm_k8s_happy" = "yes"], [$1], [$2])
])dnl
