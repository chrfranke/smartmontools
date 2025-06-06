#!/bin/sh
#
# cppcheck.sh - run cppcheck on smartmontools $srcdir
#
# Home page of code is: https://www.smartmontools.org
#
# Copyright (C) 2019-23 Christian Franke
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# $Id: cppcheck.sh 5597 2024-01-24 10:30:14Z chrfranke $
#

set -e

myname=$0

usage()
{
  echo "Usage: $myname [-v|-q] [-c CPPCHECK] [-jJOBS] [--library=CFG] [--platform=TYPE] [FILE ...]"
  exit 1
}

# Parse options
jobs=
v=
cppcheck="cppcheck"
library="--library=posix"
platform="--platform=unix64"
unused_func=",unusedFunction"

while true; do case $1 in
  -c) shift; test -n "$1" || usage; cppcheck=$1 ;;
  -j?*) jobs=$1; unused_func= ;;
  -q) v="-q" ;;
  -v) v="-v" ;;
  --platform=*) platform=$1 ;;
  --library=*) library=$1 ;;
  -*) usage ;;
  *) break ;;
esac; shift; done

# Set file list from command line or $srcdir
if [ $# -ne 0 ]; then
  files=$(echo "$@")
  files_v=$files
  unused_func=
else
  srcdir=${myname%/*}
  if [ "$srcdir" = "$myname" ]; then
    echo "$myname: \$srcdir not found" >&2
    exit 1
  fi
  cd "$srcdir" || exit 1
  files_v="*.cpp *.h os_win32/*.cpp os_win32/*.h"
  files=$(echo $files_v)
  case $files in
    *\**) echo "$myname: Not run from \$srcdir" >&2; exit 1 ;;
  esac
fi

# Check cppcheck version
ver=$("$cppcheck" --version) || exit 1
ver=${ver##* }
case $ver in
  1.8[56]|2.[237]|2.1[01]) ;;
  *) echo "$myname: cppcheck $ver not tested with this script" ;;
esac

# Build cppcheck settings
enable="warning,style,performance,portability,information${unused_func}"

sup_list="
  #warning
  syntaxError:drivedb.h
  #style
  asctime_rCalled:utility.cpp
  asctime_sCalled:utility.cpp
  cstyleCast:sg_unaligned.h
  getgrgidCalled:popen_as_ugid.cpp
  getgrnamCalled:popen_as_ugid.cpp
  getpwnamCalled:popen_as_ugid.cpp
  getpwuidCalled:popen_as_ugid.cpp
  readdirCalled
  strtokCalled
  unusedStructMember
  unusedFunction:sg_unaligned.h
  unmatchedSuppression
"

case $ver in
  2.1[1-9]) sup_list="$sup_list
  #error
  ctuOneDefinitionRuleViolation:cissio_freebsd.h
  ctuOneDefinitionRuleViolation:freebsd_nvme_ioctl.h
  #information
  missingInclude
  missingIncludeSystem
" ;;
esac

suppress=
for s in $sup_list; do
  case $s in
    \#*) continue ;;
    unusedFunction:*) test -n "$unused_func" || continue ;;
    unmatchedSuppression) test $# -ne 0 || continue ;;
  esac
  suppress="${suppress}${suppress:+ }--suppress=${s%%#*}"
done

# shellcheck disable=SC2089
defs="\
  -U__KERNEL__
  -U__LP64__
  -U__MINGW64_VERSION_STR
  -U__VERSION__
  -U_NETWARE
  -DBUILD_INFO=\"(...)\"
  -UCLOCK_MONOTONIC
  -DENOTSUP=1
  -DHAVE_ATTR_PACKED
  -DHAVE_CONFIG_H
  -DPACKAGE_VERSION=\"7.4\"
  -DSG_IO=1
  -DSMARTMONTOOLS_BUILD_HOST=\"host\"
  -DSMARTMONTOOLS_ATTRIBUTELOG=\"/file\"
  -DSMARTMONTOOLS_SAVESTATES=\"/file\"
  -DSMARTMONTOOLS_DRIVEDBDIR=\"/dir\"
  -USMARTMONTOOLS_RELEASE_DATE
  -USMARTMONTOOLS_RELEASE_TIME
  -USMARTMONTOOLS_SVN_REV
  -DSOURCE_DATE_EPOCH=1665402854
  -Umakedev
  -Ustricmp"

# Print brief version of command
cat <<EOF
# Cppcheck $ver
$cppcheck \\
  --enable=$enable \\
  $library \\
  $platform \\
  ... \\
$(echo "$defs" | sed 's,$, \\,')
$(for s in $suppress; do echo "  $s \\"; done)
  $files_v
EOF

# Run cppcheck with swapped stdout<>stderr
# shellcheck disable=SC2090
"$cppcheck" \
  $v \
  $jobs \
  --enable="$enable" \
  --template='{file}:{line}: {severity}: ({id}) {message}' \
  --force \
  --inline-suppr \
  --language=c++ \
  $library \
  $platform \
  $defs \
  $suppress \
  $files \
  3>&2 2>&1 1>&3 3>&-
