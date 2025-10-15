#
# Copyright (C) Intel Corporation, 2023-2024. ALL RIGHTS RESERVED.
# See file LICENSE for terms.
#

UCX_CHECK_ZE

AS_IF([test "x$ze_happy" = "xyes"], [uct_modules="${uct_modules}:ze"])
uct_ze_modules=""
AC_DEFINE_UNQUOTED([uct_ze_MODULES], ["${uct_ze_modules}"], [ZE loadable modules])
AC_CONFIG_FILES([src/uct/ze/Makefile
                 src/uct/ze/ucx-ze.pc])
