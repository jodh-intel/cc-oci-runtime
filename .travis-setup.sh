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

# Build runtime dependencies.
#
# Strategy:
#
# Travis expects a job to produce output. If it doesn't see any output
# for a few minutes the job will be failed. But Travis also has a
# restriction on the maximum amount of output a job can produce
# (currently 4MB).
#
# This causes a problem since some of these dependencies (particularly
# gcc) produce a lot of output *and* take a long time to build.
#
# To try to counter this, the builds are run through "pv" which outputs a
# few bytes of status every minute to keep Travis happy, but avoids the
# builds getting flooded with output. Since piping a command to "pv"
# means the shell cannot provide the exit code of the piped command, the
# strategy is to run commands that will produce a lot of output using
# the run_cmd() function which runs the command in the background and
# then connects pv to the command to monitor output.

set -e -x

# Run a command line, redirecting all output to a logfile.
# If the command fails, display the end of the logfile.
#
# Arguments:
#
# 1: command line to execute
# 2: logfile to redirect command output to
run_cmd()
{
    local cmd="$1"
    local logfile="$2"
    local pid

    eval "$cmd" > "$logfile" 2>&1 &
    pid=$!

    # connect to pid and show data progress every minute
    # (Note: "|| :" in case the pid exits before pv starts).
    pv -i 60 -d "$pid" || :

    # get the return code of the background process
    { wait "$pid"; ret=$?; } || :

    [ $ret -eq 0 ] && return 0

    tail -n 100 "${logfile}"

    # fail the job
    return 1
}

cwd=$(cd `dirname "$0"`; pwd -P)

# Contains the latest build dependency versions.
local_versions="$cwd/versions.txt"

# Contains the cached build dependency versions.
#
# Note that a better approach might be to run "pkg-config
# --print-provides" and compare that output with the version found in
# $local_versions. However:
#
# - pkg-config requires the "package name" be provided for each query.
#
#   We could hard-code that in each call to pkg-config, but a better
#   approach would be to enhance versions.txt so that it's multi-field,
#   allowing the package name to also be specified. But that would
#   require further updates to configure.ac.
#
# - pkg-config only works for libraries (and we have binaries to deal
#   with binaries too).
#
#   We could handle libraries separately via
#   "$binary --version|grep verion", but that's not reliable
#   (atleast for qemu-lite).

cached_versions="/usr/local/lib/cached_versions.txt"

source $cwd/versions.txt
[ -e "${cached_versions}" ] && source "$cached_versions"

# Ensure "make install" as root can find clang
#
# See: https://github.com/travis-ci/travis-ci/issues/2607
export CC=$(which "$CC")

gnome_dl=https://download.gnome.org/sources

# Install required dependencies to build
# glib, json-glib, libmnl-dev check and cc-oci-runtime
sudo apt-get -qq install valgrind lcov uuid-dev pkg-config \
  zlib1g-dev libffi-dev gettext libpcre3-dev cppcheck \
  libmnl-dev libcap-ng-dev libgmp-dev libmpfr-dev libmpc-dev \
  libpixman-1-dev rpm2cpio pv

mkdir cor-dependencies
pushd cor-dependencies

# "pv" itself needs to be built since the version available in Trusty
# does not support the "-d" option.
if [ ! -x /usr/local/bin/pv ]
then
    curl -L -O "http://www.ivarch.com/programs/sources/pv-${pv_version}.tar.gz"
    tar xvf "pv-${pv_version}.tar.gz"
    pushd "pv-${pv_version}"
    ./configure && make && sudo make install
    popd
    rm -rf "pv-${pv_version}" &
fi

# Build glib
if [ "${glib_version}" != "${cached_glib_version}" ]
then
    glib_major=`echo $glib_version | cut -d. -f1`
    glib_minor=`echo $glib_version | cut -d. -f2`
    curl -L -O "$gnome_dl/glib/${glib_major}.${glib_minor}/glib-${glib_version}.tar.xz"
    tar -xf "glib-${glib_version}.tar.xz"
    pushd "glib-${glib_version}"
    ./configure --disable-silent-rules
    run_cmd "make -j5" "glib-${glib_version}.log"
    sudo make install
    popd
    rm -rf "glib-${glib_version}" &
fi

# Build json-glib
if [ "${json_glib_version}" != "${cached_json_glib_version}" ]
then
    json_major=`echo $json_glib_version | cut -d. -f1`
    json_minor=`echo $json_glib_version | cut -d. -f2`
    curl -L -O "$gnome_dl/json-glib/${json_major}.${json_minor}/json-glib-${json_glib_version}.tar.xz"
    tar -xf "json-glib-${json_glib_version}.tar.xz"
    pushd "json-glib-${json_glib_version}"
    ./configure --disable-silent-rules
    run_cmd "make -j5" "json-glib-${json_glib_version}.log"
    sudo make install
    rm -rf "json-glib-${json_glib_version}" &
popd
fi

# Build check
# We need to build check as the check version in the OS used by travis isn't
# -pedantic safe.
if [ "${check_version}" != "${cached_check_version}" ]
then
    curl -L -O "https://github.com/libcheck/check/releases/download/${check_version}/check-${check_version}.tar.gz"
    tar -xf "check-${check_version}.tar.gz"
    pushd "check-${check_version}"
    ./configure
    run_cmd "make -j5" "check-${check_version}.log"
    sudo make install
    popd
    rm -rf "check-${check_version}" &
fi

# Install bats
# (Note: no version, just check if the binary is there)
if [ ! -x /usr/local/bin/bats ]
then
    git clone https://github.com/sstephenson/bats.git
    pushd bats
    sudo ./install.sh /usr/local
    popd
    rm -rf bats &
fi

# build gcc (required for qemu-lite)
export PATH="/usr/local/gcc-${gcc_version}:$PATH"
if [ "${gcc_version}" != "${cached_gcc_version}" ]
then
    curl -L -O "http://mirrors.kernel.org/gnu/gcc/gcc-${gcc_version}/gcc-${gcc_version}.tar.bz2"
    tar xf "gcc-${gcc_version}.tar.bz2"
    pushd "gcc-${gcc_version}"
    ./configure \
        --enable-languages=c \
        -disable-multilib \
        --disable-libstdcxx \
        --disable-bootstrap \
        --disable-nls \
        --prefix="/usr/local/gcc-${gcc_version}"
    run_cmd "make -j5" "gcc-${gcc_version}.log"
    sudo make install
    popd
    rm -rf "gcc-${gcc_version}" &
fi

# build qemu-lite
if [ "${qemu_lite_version}" != "${cached_qemu_lite_version}" ]
then
    curl -L -O "https://github.com/01org/qemu-lite/archive/${qemu_lite_version}.tar.gz"
    tar xf "${qemu_lite_version}.tar.gz"
    pushd "qemu-lite-${qemu_lite_version}"
    CC="/usr/local/gcc-${gcc_version}/bin/gcc" ./configure \
        --disable-bluez \
        --disable-brlapi \
        --disable-bzip2 \
        --disable-curl \
        --disable-curses \
        --disable-debug-tcg \
        --disable-fdt \
        --disable-glusterfs \
        --disable-gtk \
        --disable-libiscsi \
        --disable-libnfs \
        --disable-libssh2 \
        --disable-libusb \
        --disable-linux-aio \
        --disable-lzo \
        --disable-opengl \
        --disable-qom-cast-debug \
        --disable-rbd \
        --disable-rdma \
        --disable-sdl \
        --disable-seccomp \
        --disable-slirp \
        --disable-snappy \
        --disable-spice \
        --disable-strip \
        --disable-tcg-interpreter \
        --disable-tcmalloc \
        --disable-tools \
        --disable-tpm \
        --disable-usb-redir \
        --disable-uuid \
        --disable-vnc \
        --disable-vnc-{jpeg,png,sasl} \
        --disable-vte \
        --disable-xen \
        --enable-attr \
        --enable-cap-ng \
        --enable-kvm \
        --enable-virtfs \
        --target-list=x86_64-softmmu \
        --extra-cflags="-fno-semantic-interposition -O3 -falign-functions=32" \
        --prefix=/usr/local \
        --datadir=/usr/local/share/qemu-lite \
        --libdir=/usr/local/lib64/qemu-lite \
        --libexecdir=/usr/local/libexec/qemu-lite
    run_cmd "make -j5" "qemu-lite-${qemu_lite_version}.log"
    sudo make install
    popd
    rm -rf "qemu-lite-${qemu_lite_version}" &
fi

# install kernel + Clear Containers image
mkdir artifacts
pushd artifacts
clr_release=$(curl -L https://download.clearlinux.org/latest)
clr_kernel_base_url="https://download.clearlinux.org/releases/${clr_release}/clear/x86_64/os/Packages"

clr_assets_dir=/usr/share/clear-containers
sudo mkdir -p "$clr_assets_dir"

# find newest containers kernel
clr_kernel=$(curl -l -s -L "${clr_kernel_base_url}" |\
    grep -o "linux-container-[0-9][0-9.-]*\.x86_64.rpm" |\
    sort -u)

# download kernel
if [ ! -e "${clr_assets_dir}/${clr_kernel}" ]
then
    curl -L -O "${clr_kernel_base_url}/${clr_kernel}"

    # install kernel
    # (note: cpio on trusty does not support "-D")
    rpm2cpio "${clr_kernel}"| (cd / && sudo cpio -idv)
fi

clr_image_url="https://download.clearlinux.org/current/clear-${clr_release}-containers.img.xz"
clr_image_compressed=$(basename "$clr_image_url")

# uncompressed image name
clr_image=${clr_image_compressed/.xz/}

# download image
if [ ! -e "${clr_assets_dir}/${clr_image}" ]
then
    for file in "${clr_image_url}-SHA512SUMS" "${clr_image_url}"
    do
        curl -L -O "$file"
    done
    
    # verify image
    sha512sum -c "${clr_image_compressed}-SHA512SUMS"
    
    # unpack image
    unxz "${clr_image_compressed}"
    
    # install image
    sudo install "${clr_image}" "${clr_assets_dir}"
fi

# change kernel+image ownership
sudo chown -R $USER "${clr_assets_dir}"

# create image symlink (kernel will already have one)
clr_image_link=clear-containers.img
if [ ! -e "${clr_assets_dir}/${clr_image_link}" ]
then
    (cd "${clr_assets_dir}" && sudo ln -s "${clr_image}" "${clr_image_link}")
fi

mkdir docker
pushd docker

# create an ubuntu container
sudo docker run ubuntu true

# flatten the just-created ubuntu container
sudo docker export $(sudo docker ps -n 1 -q) | sudo docker import - cc-test-image

# export the container as a tar file
sudo docker save cc-test-image > cc-test-image.tar

# extra the container files
tar xvf cc-test-image.tar

# find the filesystem layer
# (only 1 layer as the image has been flattened)
fs=$(find . -name layer.tar)

# extract container image
mkdir -p /tmp/bundle/rootfs
sudo tar -C /tmp/bundle/rootfs -xvf "$fs"
popd

popd

# Copy the "versions database" to the cache directory if it's not
# already there. Since /usr/local is our Travis cache directory, we can
# identify on the next build if these build dependencies need to be
# rebuilt. Most of time they won't (but will if a version is bumped in
# the versions database).
sudo mkdir -p $(dirname "${cached_versions}")
egrep -v "^(\#|$)" "${local_versions}" |\
    sed 's/^/cached_/g' |\
    sudo tee "${cached_versions}"
sudo chmod 444 "${cached_versions}"
