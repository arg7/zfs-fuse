#include <sys/zfs_context.h>
#include <sys/alloc_bias.h>
#include <sys/vdev_impl.h>

typedef struct ab_engine_node {
	avl_node_t		abn_node;
	alloc_bias_ops_t	*abn_ops;
} ab_engine_node_t;

static kmutex_t ab_engine_lock;
static avl_tree_t ab_engine_tree;
static boolean_t ab_engine_initialized = B_FALSE;

static int
ab_engine_compare(const void *lhs, const void *rhs)
{
	const ab_engine_node_t *ln = lhs;
	const ab_engine_node_t *rn = rhs;
	const char *lname = ln->abn_ops->abo_name;
	const char *rname = rn->abn_ops->abo_name;
	int cmp = strcmp(lname, rname);

	if (cmp != 0)
		return (cmp);

	if (ln->abn_ops < rn->abn_ops)
		return (-1);
	if (ln->abn_ops > rn->abn_ops)
		return (1);

	return (0);
}

static void
ab_engine_tree_init(void)
{
	if (ab_engine_initialized)
		return;

	mutex_init(&ab_engine_lock, NULL, MUTEX_DEFAULT, NULL);
	avl_create(&ab_engine_tree, ab_engine_compare,
	    sizeof (ab_engine_node_t),
	    offsetof(ab_engine_node_t, abn_node));
	ab_engine_initialized = B_TRUE;
}

static ab_engine_node_t *
ab_engine_find_locked(const char *name)
{
	ab_engine_node_t search;
	alloc_bias_ops_t fake_ops;

	fake_ops.abo_name = name;
	search.abn_ops = &fake_ops;

	return (avl_find(&ab_engine_tree, &search, NULL));
}

int
ab_register_engine(alloc_bias_ops_t *ops)
{
	if (ops == NULL || ops->abo_name == NULL)
		return (EINVAL);

	ab_engine_tree_init();

	mutex_enter(&ab_engine_lock);

	if (ab_engine_find_locked(ops->abo_name) != NULL) {
		mutex_exit(&ab_engine_lock);
		return (EEXIST);
	}

	ab_engine_node_t *node = kmem_zalloc(sizeof (*node), KM_SLEEP);
	node->abn_ops = ops;
	avl_add(&ab_engine_tree, node);

	mutex_exit(&ab_engine_lock);

	return (0);
}

void
ab_deregister_engine(alloc_bias_ops_t *ops)
{
	if (ops == NULL || !ab_engine_initialized)
		return;

	mutex_enter(&ab_engine_lock);

	ab_engine_node_t *node = ab_engine_find_locked(ops->abo_name);
	if (node != NULL && node->abn_ops == ops) {
		avl_remove(&ab_engine_tree, node);
		kmem_free(node, sizeof (*node));
	}

	mutex_exit(&ab_engine_lock);
}

alloc_bias_ops_t *
ab_find_engine_by_name(const char *name)
{
	alloc_bias_ops_t *ops = NULL;

	if (!ab_engine_initialized || name == NULL)
		return (NULL);

	mutex_enter(&ab_engine_lock);

	ab_engine_node_t *node = ab_engine_find_locked(name);
	if (node != NULL)
		ops = node->abn_ops;

	mutex_exit(&ab_engine_lock);

	return (ops);
}

static int
ab_context_compare(const void *lhs, const void *rhs)
{
	const alloc_bias_context_t *lc = lhs;
	const alloc_bias_context_t *rc = rhs;

	if (lc->abc_ops < rc->abc_ops)
		return (-1);
	if (lc->abc_ops > rc->abc_ops)
		return (1);

	if (lc->abc_primary_key < rc->abc_primary_key)
		return (-1);
	if (lc->abc_primary_key > rc->abc_primary_key)
		return (1);

	if (lc->abc_stream_id < rc->abc_stream_id)
		return (-1);
	if (lc->abc_stream_id > rc->abc_stream_id)
		return (1);

	if (lc < rc)
		return (-1);
	if (lc > rc)
		return (1);

	return (0);
}

void
ab_vdev_init(vdev_t *vd)
{
	mutex_init(&vd->vdev_alloc_bias_lock, NULL, MUTEX_DEFAULT, NULL);
	avl_create(&vd->vdev_alloc_bias_contexts, ab_context_compare,
	    sizeof (alloc_bias_context_t),
	    offsetof(alloc_bias_context_t, abc_node));
}

void
ab_vdev_fini(vdev_t *vd)
{
	alloc_bias_context_t *ctx;
	void *cookie = NULL;

	mutex_enter(&vd->vdev_alloc_bias_lock);
	while ((ctx = avl_destroy_nodes(&vd->vdev_alloc_bias_contexts,
	    &cookie)) != NULL) {
		kmem_free(ctx, sizeof (*ctx) + ctx->abc_ops->abo_private_ctx_size);
	}
	avl_destroy(&vd->vdev_alloc_bias_contexts);
	mutex_exit(&vd->vdev_alloc_bias_lock);

	mutex_destroy(&vd->vdev_alloc_bias_lock);
}

alloc_bias_context_t *
ab_context_alloc(alloc_bias_ops_t *ops, uint64_t primary_key,
	uint64_t stream_id)
{
	size_t ctx_size = sizeof (alloc_bias_context_t) +
	    ops->abo_private_ctx_size;
	alloc_bias_context_t *ctx = kmem_zalloc(ctx_size, KM_SLEEP);

	ctx->abc_ops = ops;
	ctx->abc_primary_key = primary_key;
	ctx->abc_stream_id = stream_id;

	return (ctx);
}

alloc_bias_context_t *
ab_context_lookup(vdev_t *vd, alloc_bias_ops_t *ops,
	uint64_t primary_key, uint64_t stream_id)
{
	alloc_bias_context_t search;

	ASSERT(MUTEX_HELD(&vd->vdev_alloc_bias_lock));

	bzero(&search, sizeof (search));
	search.abc_ops = ops;
	search.abc_primary_key = primary_key;
	search.abc_stream_id = stream_id;

	return (avl_find(&vd->vdev_alloc_bias_contexts, &search, NULL));
}

void
ab_context_insert(vdev_t *vd, alloc_bias_context_t *ctx)
{
	ASSERT(MUTEX_HELD(&vd->vdev_alloc_bias_lock));
	avl_add(&vd->vdev_alloc_bias_contexts, ctx);
}

void
ab_context_remove(vdev_t *vd, alloc_bias_context_t *ctx)
{
	ASSERT(MUTEX_HELD(&vd->vdev_alloc_bias_lock));
	avl_remove(&vd->vdev_alloc_bias_contexts, ctx);
	kmem_free(ctx, sizeof (*ctx) + ctx->abc_ops->abo_private_ctx_size);
}
