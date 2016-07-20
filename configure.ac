AC_PREREQ(2.61)
 AC_INIT([binder], 1.0.0)
 AM_INIT_AUTOMAKE([-Wall gnu foreign])
 AC_CONFIG_SRCDIR([Makefile.am])
 AC_CONFIG_HEADERS([config.h])
 AC_CONFIG_MACRO_DIR([m4])
 
 # Check for programs
 LT_INIT
 AC_PROG_LIBTOOL
 AC_PROG_CXX
 AC_PROG_CC
 AM_PROG_CC_C_O
 AC_PROG_AWK
 AC_PROG_CPP
 AC_PROG_INSTALL
 AC_PROG_LN_S
 AC_PROG_MAKE_SET
 PKG_PROG_PKG_CONFIG
 
 AS_CASE([$host],
         [arm*], [ARM=yes],
         [ARM=no]
 )
 
 ARM=yes
 
 AC_ARG_WITH([core_includes],
       AC_HELP_STRING([--with-core-includes=@<:@dir@:>@],
          [Specify the location of the core headers]),
       [core_incdir=$withval],
       with_core_includes=no)
 
 if test "x$with_core_includes" != "xno"; then
      CPPFLAGS="${CPPFLAGS} -I${core_incdir}"
      AC_SUBST(core_incdir,${core_incdir})
 fi
 
 AC_SUBST([CFLAGS])
 AC_SUBST([CPPFLAGS])
 AC_SUBST([LIBS])
 
 AC_CONFIG_FILES([ \
         Makefile  \
         libs/ui/Makefile \
         libs/ui/libui.pc \
         include/Makefile
         ])
 AC_OUTPUT
