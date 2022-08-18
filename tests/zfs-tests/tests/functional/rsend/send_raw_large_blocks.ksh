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
# Copyright (c) 2022, George Amanakis. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/rsend/rsend.kshlib

#
# Description:
# Receiving a snapshot with large blocks and raw sending it succeeds.
#
# Strategy:
# 1) Create a set of files each containing some file data in an
#	encrypted filesystem with recordsize=1m.
# 2) Snapshot and send with large_blocks enabled to a new filesystem.
# 3) Raw send to a file. If the large_blocks feature is not activated
#	in the filesystem created in (2) the raw send will fail.
#

verify_runnable "both"

log_assert "Receiving and raw sending a snapshot with large blocks succeeds"

DISK=${DISKS%% *}

backup=$TEST_BASE_DIR/backup
raw_backup=$TEST_BASE_DIR/raw_backup

function cleanup
{
        log_must rm -f $backup $raw_backup $ibackup $unc_backup
	destroy_dataset $POOL/fs "-rR"
}

log_onexit cleanup

typeset passphrase="password"
typeset file="/$POOL/fs/$TESTFILE0"

log_must eval "echo $passphrase > /$POOL/pwd"

log_must zfs create -o recordsize=1m $POOL/fs
log_must dd if=/dev/random of=$file bs=1024 count=1024 oflag=sync
log_must zfs snapshot $POOL/fs@snap1

log_must eval "zfs send -L $POOL/fs@snap1 > $backup"
log_must eval "zfs recv -o encryption=aes-256-ccm -o keyformat=passphrase \
    -o keylocation=file:///$POOL/pwd -o primarycache=none \
    -o recordsize=1m $POOL/testfs5 < $backup"

log_must eval "zfs send --raw $POOL/testfs5@snap1 > $raw_backup"

log_pass "Receiving and raw sending a snapshot with large blocks succeeds"
