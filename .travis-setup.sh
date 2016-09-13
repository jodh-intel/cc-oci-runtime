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

cwd=$(cd `dirname "$0"`; pwd -P)
source $cwd/versions.txt

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
  libpixman-1-dev rpm2cpio

mkdir cor-dependencies
pushd cor-dependencies

# Build glib
glib_major=`echo $glib_version | cut -d. -f1`
glib_minor=`echo $glib_version | cut -d. -f2`
curl -L -O "$gnome_dl/glib/${glib_major}.${glib_minor}/glib-${glib_version}.tar.xz"
tar -xf "glib-${glib_version}.tar.xz"
pushd "glib-${glib_version}"
./configure --disable-silent-rules
make -j5
sudo make install
popd

# Build json-glib
json_major=`echo $json_glib_version | cut -d. -f1`
json_minor=`echo $json_glib_version | cut -d. -f2`
curl -L -O "$gnome_dl/json-glib/${json_major}.${json_minor}/json-glib-${json_glib_version}.tar.xz"
tar -xf "json-glib-${json_glib_version}.tar.xz"
pushd "json-glib-${json_glib_version}"
./configure --disable-silent-rules
make -j5
sudo make install
popd

# Build check
# We need to build check as the check version in the OS used by travis isn't
# -pedantic safe.
curl -L -O "https://github.com/libcheck/check/releases/download/${check_version}/check-${check_version}.tar.gz"
tar -xf "check-${check_version}.tar.gz"
pushd "check-${check_version}"
./configure
make -j5
sudo make install
popd


# Install bats
git clone https://github.com/sstephenson/bats.git
pushd bats
sudo ./install.sh /usr/local
popd

# build gcc (required for qemu-lite)
curl -L -O "http://mirrors.kernel.org/gnu/gcc/gcc-${gcc_version}/gcc-${gcc_version}.tar.bz2"
tar xf "gcc-${gcc_version}.tar.bz2"
pushd "gcc-${gcc_version}"
./configure --enable-languages=c --disable-multilib --prefix="/usr/local/gcc-${gcc_version}"
make -j5
sudo make install
export PATH="/usr/local/gcc-${gcc_version}:$PATH"
popd

# build qemu-lite
curl -L -O "https://github.com/01org/qemu-lite/archive/${qemu_lite_version}.tar.gz"
tar xf "${qemu_lite_version}.tar.gz"
pushd "qemu-lite-${qemu_lite_version}"
CC="gcc" ./configure \
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
make -j5
sudo make install
popd

# install kernel + Clear Containers image
mkdir artifacts
pushd artifacts
clr_release=$(curl -L https://download.clearlinux.org/latest)
clr_kernel_base_url="https://download.clearlinux.org/releases/${clr_release}/clear/x86_64/os/Packages"
sudo mkdir -p /usr/share/clear-containers/

# find newest containers kernel
kernel=$(curl -l -s -L "${clr_kernel_base_url}" |\
    grep -o "linux-container-[0-9][0-9.-]*\.x86_64.rpm" |\
    sort -u)

# download kernel
curl -L -O "${clr_kernel_base_url}/${kernel}"

# install kernel
# (note: cpio on trusty does not support "-D")
rpm2cpio "$kernel"| (cd / && sudo cpio -idv)

# download image
clr_image_url="https://download.clearlinux.org/current/clear-${clr_release}-containers.img.xz"
clr_image=$(basename "$clr_image_url")
for file in "${clr_image_url}-SHA512SUMS" "${clr_image_url}"
do
    curl -L -O "$file"
done

# verify image
sha512sum -c "${clr_image}-SHA512SUMS"

# unpack image
unxz "${clr_image}"

# remove suffix
clr_image=${clr_image/.xz/}

# install image
sudo install "${clr_image}" /usr/share/clear-containers/

# change kernel+image ownership
sudo chown -R $USER /usr/share/clear-containers/

# create image symlink (kernel will already have one)
(cd /usr/share/clear-containers && sudo ln -s "${clr_image}" clear-containers.img)

popd
