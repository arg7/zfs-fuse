/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef _SYS_METASLAB_H
#define	_SYS_METASLAB_H

#include <sys/spa.h>
#include <sys/space_map.h>
#include <sys/txg.h>
#include <sys/zio.h>
#include <sys/avl.h>
#include <sys/alloc_bias.h>
#include <sys/alloc_bias_backend.h>

#ifdef	__cplusplus
extern "C" {
#endif

extern space_map_ops_t *zfs_metaslab_ops;

extern metaslab_t *metaslab_init(metaslab_group_t *mg, space_map_obj_t *smo,
    uint64_t start, uint64_t size, uint64_t txg);
extern void metaslab_fini(metaslab_t *msp);
extern void metaslab_sync(metaslab_t *msp, uint64_t txg);
extern void metaslab_sync_done(metaslab_t *msp, uint64_t txg);
extern void metaslab_sync_reassess(metaslab_group_t *mg);

#define	METASLAB_HINTBP_FAVOR	0x0
#define	METASLAB_HINTBP_AVOID	0x1
#define	METASLAB_GANG_HEADER	0x2

extern int metaslab_alloc(spa_t *spa, metaslab_class_t *mc, uint64_t psize,
    blkptr_t *bp, int ncopies, uint64_t txg, blkptr_t *hintbp, int flags,
    dmu_object_type_t obj_type, zio_t *zio);
extern void metaslab_free(spa_t *spa, const blkptr_t *bp, uint64_t txg,
    boolean_t now);
extern int metaslab_claim(spa_t *spa, const blkptr_t *bp, uint64_t txg);

extern const vdev_alloc_backend_ops_t *metaslab_get_alloc_backend_ops(void);

extern metaslab_class_t *metaslab_class_create(spa_t *spa,
    space_map_ops_t *ops);
extern void metaslab_class_destroy(metaslab_class_t *mc);
extern int metaslab_class_validate(metaslab_class_t *mc);

extern void metaslab_class_space_update(metaslab_class_t *mc,
    int64_t alloc_delta, int64_t defer_delta,
    int64_t space_delta, int64_t dspace_delta);
extern uint64_t metaslab_class_get_alloc(metaslab_class_t *mc);
extern uint64_t metaslab_class_get_space(metaslab_class_t *mc);
extern uint64_t metaslab_class_get_dspace(metaslab_class_t *mc);
extern uint64_t metaslab_class_get_deferred(metaslab_class_t *mc);

extern metaslab_group_t *metaslab_group_create(metaslab_class_t *mc,
    vdev_t *vd);
extern void metaslab_group_destroy(metaslab_group_t *mg);
extern void metaslab_group_activate(metaslab_group_t *mg);
extern void metaslab_group_passivate(metaslab_group_t *mg);

//extern uint64_t obj_alloc_class(dmu_object_type_t ot);

#define  alloc_bias_max 8

static alloc_bias_context_t alloc_bias_contexts[alloc_bias_max] = {};

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_METASLAB_H */
