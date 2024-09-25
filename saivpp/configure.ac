AC_INIT([saivpp],[1.0])
AC_CONFIG_SRCDIR([])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AC_LANG_C
AC_LANG([C++])
AC_PROG_CC
AC_PROG_CXX
AC_PROG_LIBTOOL
AC_HEADER_STDC
AX_CODE_COVERAGE
AX_ADD_AM_MACRO_STATIC([])

LDFLAGS="-Wl,--no-undefined $LDFLAGS"
AC_SUBST([LDFLAGS])

AC_SUBST(SAIINC, "-I\$(top_srcdir)/SAI/inc -I\$(top_srcdir)/SAI/experimental -I\$(top_srcdir)/SAI/meta")

AC_ARG_ENABLE(debug,
[  --enable-debug       Compile with debugging flags],
[case "${enableval}" in
	yes) debug=true ;;
	no)  debug=false ;;
	*) AC_MSG_ERROR(bad value ${enableval} for --enable-debug) ;;
esac],[debug=false])
AM_CONDITIONAL(DEBUG, test x$debug = xtrue)

CXXFLAGS_COMMON=""
CXXFLAGS_COMMON+=" -ansi"
CXXFLAGS_COMMON+=" -fPIC"
CXXFLAGS_COMMON+=" -std=c++14"
CXXFLAGS_COMMON+=" -Wall"
CXXFLAGS_COMMON+=" -Wcast-align"
CXXFLAGS_COMMON+=" -Wcast-qual"
CXXFLAGS_COMMON+=" -Wconversion"
CXXFLAGS_COMMON+=" -Wdisabled-optimization"
CXXFLAGS_COMMON+=" -Wextra"
CXXFLAGS_COMMON+=" -Wfloat-equal"
CXXFLAGS_COMMON+=" -Wformat=2"
CXXFLAGS_COMMON+=" -Wformat-nonliteral"
CXXFLAGS_COMMON+=" -Wformat-security"
CXXFLAGS_COMMON+=" -Wformat-y2k"
CXXFLAGS_COMMON+=" -Wimport"
CXXFLAGS_COMMON+=" -Winit-self"
CXXFLAGS_COMMON+=" -Winvalid-pch"
CXXFLAGS_COMMON+=" -Wlong-long"
CXXFLAGS_COMMON+=" -Wmissing-field-initializers"
CXXFLAGS_COMMON+=" -Wmissing-format-attribute"
CXXFLAGS_COMMON+=" -Wmissing-include-dirs"
CXXFLAGS_COMMON+=" -Wmissing-noreturn"
CXXFLAGS_COMMON+=" -Wno-aggregate-return"
CXXFLAGS_COMMON+=" -Wno-padded"
CXXFLAGS_COMMON+=" -Wno-switch-enum"
CXXFLAGS_COMMON+=" -Wno-unused-parameter"
CXXFLAGS_COMMON+=" -Wpacked"
CXXFLAGS_COMMON+=" -Wpointer-arith"
CXXFLAGS_COMMON+=" -Wredundant-decls"
CXXFLAGS_COMMON+=" -Wshadow"
CXXFLAGS_COMMON+=" -Wstack-protector"
CXXFLAGS_COMMON+=" -Wstrict-aliasing=3"
CXXFLAGS_COMMON+=" -Wswitch"
CXXFLAGS_COMMON+=" -Wswitch-default"
CXXFLAGS_COMMON+=" -Wunreachable-code"
CXXFLAGS_COMMON+=" -Wunused"
CXXFLAGS_COMMON+=" -Wvariadic-macros"
CXXFLAGS_COMMON+=" -Wno-write-strings"
CXXFLAGS_COMMON+=" -Wno-missing-format-attribute"
CXXFLAGS_COMMON+=" -Wno-long-long"
CXXFLAGS_COMMON+=" -fstack-protector-strong"

AC_SUBST(CXXFLAGS_COMMON)

AC_CONFIG_FILES([
    Makefile
    src/Makefile
])

AC_OUTPUT