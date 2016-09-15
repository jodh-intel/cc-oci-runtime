#!/bin/bash
#  This file is part of cc-oci-runtime.
#
#  Copyright (C) 2016 Intel Corporation
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software
#  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

set -e -x

#./autogen.sh \
#    --enable-cppcheck \
#    --enable-valgrind \
#    --disable-valgrind-helgrind \
#    --disable-valgrind-drd \
#    --disable-silent-rules
#
#make -j5 CFLAGS=-Werror check

# Name of docker image used to build in
image=jodh/cor-build-fedora

# additional build flags
CFLAGS=-Werror

# arguments to pass to the build
args=""

# Clear Containers assets
args="$args --with-cc-image=/usr/share/clear-containers/clear-containers.img"
args="$args --with-cc-kernel=/usr/share/clear-containers/vmlinux.container"
args="$args --with-tests-bundle-path=/var/lib/oci/bundle"

# valgrind (memcheck) is essential, but helgrind and drd fail due to
# suspected glib thread issues.
#
# See: https://github.com/01org/cc-oci-runtime/issues/214
args="$args --enable-valgrind"
args="$args --disable-valgrind-helgrind"
args="$args --disable-valgrind-drd"

# code coverage
args="$args --enable-code-coverage"
args="$args --with-gcov=lcov"

# static analysis
args="$args --enable-cppcheck"

# miscellaneous
args="$args --disable-silent-rules"

# unlikely in cloud scenarios, but if VT-x support is available, use it! :-)
grep -q vmx /proc/cpuinfo || args="$args --without-kvm-support"

# Share the source directory into the container, configure and run the
# tests twice - once as a non-privileged user, then as root. This is
# required since the behaviour is slightly different for each.
#
# Notes:
#
# - the security option which is required to make use of ptrace(2)
#   under docker.
# - the test runs are not run multi-job since with "-j" the output is
#   intermingled and hence confusing.
docker run -ti \
    --security-opt=seccomp:unconfined \
    -v $PWD:/home/cor \
    "$image" \
    /bin/bash -c "./autogen.sh $args && \
        make -j5 CFLAGS=$CFLAGS && \
        make check && \
        make distcheck && \
        sudo make check"
