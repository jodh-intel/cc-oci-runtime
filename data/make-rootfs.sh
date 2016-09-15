#!/bin/sh
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
#

# Description: Script to create a rootfs tar file that will be copied
#   into the docker testing image and used as an OCI bundle.

set -e

# a tiny docker image that provides a useful test environment
docker_image="busybox"

die()
{
    local msg="$*"
    echo "ERROR: $msg" >&2
    exit 1
}

[ $(id -u) -eq 0 ] || die "need root"

tmp=$(mktemp -d)

# create a small container
container=$(docker create "${docker_image}")

# flatten the just-created container
name=$(docker export "$container" | docker import - 2>/dev/null)

tar="${tmp}/${name}.tar"

# export the container as a tar file
docker save "$name" > "$tar"

# remove the temporary image
docker rmi "$name" >/dev/null 2>&1

# extract the container files
tar -C "$tmp" -xvf "$tar" >/dev/null 2>&1

# find the filesystem layer
# (only 1 layer as the image has been flattened)
fs=$(find "$tmp" -name layer.tar)

rootfs="rootfs.tar.gz"

cat "$fs" |gzip -9 > "$rootfs"

# clean up
rm -rf "$tmp"

echo "rootfs is here: $rootfs"
