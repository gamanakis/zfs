#!/bin/ksh -p
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or https://opensource.org/licenses/CDDL-1.0.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright (c) 2023 George Amanakis. All rights reserved.
#

#
# DESCRIPTION:
# Verify stack overflow does not occur in persistent errlog
#
# STRATEGY:
# 1. Create a pool, and a file
# 2. Corrupt the file
# 3. Create snapshots and clones like:
# 	fs->snap1->clone1->snap2->clone2->...
# 	for a depth of 100.
# 4. Scrub to detect the corruption.
# 5. Verify we do not crash.

. $STF_SUITE/include/libtest.shlib

verify_runnable "both"

function cleanup
{
	destroy_pool $TESTPOOL2
	rm -f $TESTDIR/vdev_a
}

log_assert "Verify stack overflow does not occur in persistent errlog"
log_onexit cleanup

typeset file="/$TESTPOOL2/$TESTFS1/$TESTFILE0"

truncate -s $MINVDEVSIZE $TESTDIR/vdev_a
log_must zpool create -f $TESTPOOL2 $TESTDIR/vdev_a
log_must zfs create -o primarycache=none $TESTPOOL2/$TESTFS1
log_must dd if=/dev/urandom of=$file bs=1024 count=1024 oflag=sync
corrupt_blocks_at_level $file 0

lastfs="$(zfs list -r $TESTPOOL2 | tail -1 | awk '{print $1}')"
for i in {1..100}; do
	log_must zfs snap $lastfs@snap$i
	log_must zfs clone $lastfs@snap$i $TESTPOOL2/clone$i
	lastfs="$(zfs list -r $TESTPOOL2/clone$i | tail -1 | awk '{print $1}')"
done

log_must zpool scrub -w $TESTPOOL2
zpool status -v $TESTPOOL2
log_must eval "zpool status -v $TESTPOOL2 | \
    grep \"Kernel stack insufficient\""

log_pass "Verify stack overflow does not occur in persistent errlog"
