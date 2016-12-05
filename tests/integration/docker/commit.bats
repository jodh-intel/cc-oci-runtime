#!/usr/bin/env bats
# *-*- Mode: sh; sh-basic-offset: 8; indent-tabs-mode: nil -*-*

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

#Based on docker commands

SRC="${BATS_TEST_DIRNAME}/../../lib/"

setup() {
	source $SRC/test-common.bash
	clean_docker_ps
	runtime_docker
}

@test "Commit a container" {
	$DOCKER_EXE run -ti --name container1 busybox /bin/sh -c "echo hello"
	$DOCKER_EXE commit -m "test_commit" container1 container/test-container
	$DOCKER_EXE rmi container/test-container
}

@test "Commit a container with new configurations" {
	$DOCKER_EXE run -ti --name container2 busybox /bin/sh -c "echo hello"
	$DOCKER_EXE inspect -f "{{ .Config.Env }}" container2
	$DOCKER_EXE commit --change "ENV DEBUG true" container2 test/container-test
	$DOCKER_EXE rmi test/container-test
}
