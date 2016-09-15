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

# XXX: named version (also referenced below for the OBS elements)
FROM fedora:24

#---------------------------------------------------------------------
# XXX: Caveat Emptor - Read this!

LABEL \
    description="Image for building and running tests for \
https://github.com/01org/cc-oci-runtime/." \
    notes="This is *NOT* a general purpose image." \
    warning="Note that you must 'docker run \
--security-opt=seccomp:unconfined ...' since the runtime needs to use \
ptrace(2)."

#---------------------------------------------------------------------

# Disable deltarpms.
#
# See: https://bugzilla.redhat.com/show_bug.cgi?id=1343114)
RUN echo deltarpm=0 >> /etc/dnf/dnf.conf

# Ensure packages are current, then install:
#
# - a basic development environment
# - runtime build dependencies and additional general tooling
#
# (Note: python3-dnf-plugins-core is required for "dnf config-manager").
RUN dnf -y update && \
    dnf -y install \
        @development-tools \
        autoconf \
        automake \
        bats \
        check \
        check-devel \
        cppcheck \
        gettext-devel \
        glib2-devel \
        json-glib-devel \
        lcov \
        libffi-devel \
        libmnl-devel \
        libtool \
        libuuid-devel \
        pcre-devel \
        pkgconfig \
        python3-dnf-plugins-core \
        sudo \
        valgrind \
        zlib-devel

# Add the OBS repository
# XXX: Note the named version of Fedora
RUN dnf -y config-manager --add-repo \
    http://download.opensuse.org/repositories/home:clearlinux:preview:clear-containers-2.0/Fedora_24/home:clearlinux:preview:clear-containers-2.0.repo

# Install the Clear Containers assets.
#
# Note that the command below installs (a version of) the runtime. This
# version isn't used (since this docker image after all is for testing
# development versions of the runtime). However, specifying the runtime
# package ensures the Clear Containers assets are installed
# automatically as dependencies of the runtime package.
RUN dnf -y install cc-oci-runtime linux-container

# Create a non-privileged user (but give them root abilities via sudo).
RUN adduser --create-home --shell /bin/bash cor
RUN echo 'cor ALL=(ALL:ALL) NOPASSWD: ALL' >> /etc/sudoers
RUN chmod 600 /etc/sudoers && chown root:root /etc/sudoers

# Unpack the root image created by make-rootfs.sh into the specified
# directory.
ADD rootfs.tar.gz /var/lib/oci/bundle/rootfs

# Fix permissions for running as a non-privileged user
RUN chown cor:cor \
    /var/lib/oci/bundle \
    /var/lib/oci/bundle/rootfs \
    /usr/share/clear-containers/vmlinux.container \
    /usr/share/clear-containers/clear-containers.img

# All commands in the image will run as this user by default
USER cor 

# Provide sensible defaults
ENV HOME /home/cor
WORKDIR $HOME
