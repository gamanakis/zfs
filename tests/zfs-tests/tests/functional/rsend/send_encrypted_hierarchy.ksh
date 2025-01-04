#!/bin/ksh -p
#
# CDDL HEADER START
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#
# CDDL HEADER END
#

#
# Copyright (c) 2017 by Datto Inc. All rights reserved.
#

. $STF_SUITE/tests/functional/rsend/rsend.kshlib
. $STF_SUITE/tests/functional/cli_root/zfs_load-key/zfs_load-key_common.kshlib

#
# DESCRIPTION:
# Raw recursive sends preserve filesystem structure.
#
# STRATEGY:
# 1. Create an encrypted filesystem with a clone and a child
# 2. Snapshot and send the filesystem tree
# 3. Verify that the filesystem structure was correctly received
# 4. Change the child to an encryption root and promote the clone
# 5. Snapshot and send the filesystem tree again
# 6. Verify that the new structure is received correctly
#

verify_runnable "both"

function cleanup
{
	log_must cleanup_pool $POOL
	log_must cleanup_pool $POOL2
	log_must setup_test_model $POOL
}

log_assert "Raw recursive sends preserve filesystem structure."
log_onexit cleanup

# Create the filesystem hierarchy
log_must cleanup_pool $POOL
log_must zfs create $POOL/fs1
log_must zfs create $POOL2/fs2
log_must eval "echo $PASSPHRASE | zfs create -o encryption=on" \
	"-o keyformat=passphrase $POOL/fs1/$FS"
log_must zfs snapshot $POOL/fs1/$FS@snap
log_must zfs clone $POOL/fs1/$FS@snap $POOL/clone
log_must zfs create $POOL/fs1/$FS/child
log_must eval "echo $PASSPHRASE | zfs create -o encryption=on" \
	"-o keyformat=passphrase $POOL/fs1/a"
log_must zfs snapshot $POOL/fs1/a@snapa
log_must zfs clone $POOL/fs1/a@snapa $POOL/clonea
log_must zfs create $POOL/fs1/a/childa

# Back up the tree and verify the structure
log_must zfs snapshot -r $POOL@before
log_must eval "zfs send -wR $POOL@before > $BACKDIR/fs-before-R"
log_must eval "zfs receive -d -F $POOL2/fs2 < $BACKDIR/fs-before-R"
dstds=$(get_dst_ds $POOL/fs1/$FS $POOL2/fs2)
dstdsa=$(get_dst_ds $POOL/fs1/a $POOL2/fs2)
log_must cmp_ds_subs $POOL/fs1/$FS $dstds
log_must cmp_ds_subs $POOL/fs1/a $dstdsa

log_must verify_encryption_root $POOL2/fs2/fs1/$FS $POOL2/fs2/fs1/$FS
log_must verify_keylocation $POOL2/fs2/fs1/$FS "prompt"
log_must verify_origin $POOL2/fs2/fs1/$FS "-"
log_must verify_encryption_root $POOL2/fs2/fs1/a $POOL2/fs2/fs1/a
log_must verify_keylocation $POOL2/fs2/fs1/a "prompt"
log_must verify_origin $POOL2/fs2/fs1/a "-"

log_must verify_encryption_root $POOL2/fs2/clone $POOL2/fs2/fs1/$FS
log_must verify_keylocation $POOL2/fs2/clone "none"
log_must verify_origin $POOL2/fs2/clone "$POOL2/fs2/fs1/$FS@snap"
log_must verify_encryption_root $POOL2/fs2/clonea $POOL2/fs2/fs1/a
log_must verify_keylocation $POOL2/fs2/clonea "none"
log_must verify_origin $POOL2/fs2/clonea "$POOL2/fs2/fs1/a@snapa"

log_must verify_encryption_root $POOL/fs1/$FS/child $POOL/fs1/$FS
log_must verify_encryption_root $POOL2/fs2/fs1/$FS/child $POOL2/fs2/fs1/$FS
log_must verify_keylocation $POOL2/fs2/fs1/$FS/child "none"
log_must verify_encryption_root $POOL2/fs2/fs1/a/childa $POOL2/fs2/fs1/a
log_must verify_keylocation $POOL2/fs2/fs1/a/childa "none"

# Alter the hierarchy and re-send
log_must eval "echo $PASSPHRASE1 | zfs change-key -o keyformat=passphrase" \
	"$POOL/fs1/$FS/child"
log_must eval "echo $PASSPHRASE1 | zfs change-key -o keyformat=passphrase" \
	"$POOL/fs1/a/childa"
log_must zfs promote $POOL/clone
log_must zfs promote $POOL/clonea
log_must zfs snapshot -r $POOL@after
log_must eval "zfs send -wR -i $POOL@before $POOL@after >" \
	"$BACKDIR/fs-after-R"
log_must eval "zfs receive -d -F $POOL2/fs2 < $BACKDIR/fs-after-R"
log_must cmp_ds_subs $POOL/fs1/$FS $dstds
log_must cmp_ds_subs $POOL/fs1/a $dstdsa

log_must verify_encryption_root $POOL/fs1/$FS $POOL/clone
log_must verify_keylocation $POOL/fs1/$FS "none"
log_must verify_origin $POOL/fs1/$FS "$POOL/clone@snap"
log_must verify_encryption_root $POOL/fs1/a $POOL/clonea
log_must verify_keylocation $POOL/fs1/a "none"
log_must verify_origin $POOL/fs1/a "$POOL/clonea@snapa"

log_must verify_encryption_root $POOL/clone $POOL/clone
log_must verify_keylocation $POOL/clone "prompt"
log_must verify_origin $POOL/clone "-"
log_must verify_encryption_root $POOL/clonea $POOL/clonea
log_must verify_keylocation $POOL/clonea "prompt"
log_must verify_origin $POOL/clonea "-"

log_must verify_encryption_root $POOL/fs1/$FS/child $POOL/fs1/$FS/child
log_must verify_keylocation $POOL/fs1/$FS/child "prompt"
log_must verify_encryption_root $POOL/fs1/a/childa $POOL/fs1/a/childa
log_must verify_keylocation $POOL/fs1/a/childa "prompt"

log_must verify_encryption_root $POOL2/fs2 "-"
log_must verify_encryption_root $POOL2/fs2/clone $POOL2/fs2/clone
log_must verify_encryption_root $POOL2/fs2/clonea $POOL2/fs2/clonea
log_must verify_encryption_root $POOL2/fs2/fs1 "-"
log_must verify_encryption_root $POOL2/fs2/fs1/a $POOL2/fs2/clonea
log_must verify_encryption_root $POOL2/fs2/fs1/a/childa $POOL2/fs2/fs1/a/childa
log_must verify_encryption_root $POOL2/fs2/fs1/$FS $POOL2/fs2/clone
log_must verify_encryption_root $POOL2/fs2/fs1/$FS/child $POOL2/fs2/fs1/$FS/child

log_must verify_keylocation $POOL2/fs2 "none"
log_must verify_keylocation $POOL2/fs2/clone "prompt"
log_must verify_keylocation $POOL2/fs2/clonea "prompt"
log_must verify_keylocation $POOL2/fs2/fs1 "none"
log_must verify_keylocation $POOL2/fs2/fs1/a "none"
log_must verify_keylocation $POOL2/fs2/fs1/a/childa "prompt"
log_must verify_keylocation $POOL2/fs2/fs1/$FS "none"
log_must verify_keylocation $POOL2/fs2/fs1/$FS/child "prompt"

log_must verify_origin $POOL2/fs2 "-"
log_must verify_origin $POOL2/fs2/clone "-"
log_must verify_origin $POOL2/fs2/clonea "-"
log_must verify_origin $POOL2/fs2/fs1 "-"
log_must verify_origin $POOL2/fs2/fs1/a "$POOL2/fs2/clonea@snapa"
log_must verify_origin $POOL2/fs2/fs1/a/childa "-"
log_must verify_origin $POOL2/fs2/fs1/$FS "$POOL2/fs2/clone@snap"
log_must verify_origin $POOL2/fs2/fs1/$FS/child "-"
log_must zfs list

log_pass "Raw recursive sends preserve filesystem structure."
