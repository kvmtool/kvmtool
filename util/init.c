#include <linux/list.h>
#include <linux/kernel.h>

#include "kvm/kvm.h"
#include "kvm/util-init.h"

#define PRIORITY_LISTS 10

static struct hlist_head init_lists[PRIORITY_LISTS];
static struct hlist_head exit_lists[PRIORITY_LISTS];

int init_list_add(struct init_item *t, int (*init)(struct kvm *),
			int priority, const char *name)
{
	t->init = init;
	t->fn_name = name;
	hlist_add_head(&t->n, &init_lists[priority]);

	return 0;
}

int exit_list_add(struct init_item *t, int (*init)(struct kvm *),
			int priority, const char *name)
{
	t->init = init;
	t->fn_name = name;
	hlist_add_head(&t->n, &exit_lists[priority]);

	return 0;
}

int init_list__init(struct kvm *kvm)
{
	unsigned int i;
	int r = 0;
	struct hlist_node *n;
	struct init_item *t;

	for (i = 0; i < ARRAY_SIZE(init_lists); i++)
		hlist_for_each_entry(t, n, &init_lists[i], n) {
			r = t->init(kvm);
			if (r < 0) {
				pr_warning("Failed init: %s\n", t->fn_name);
				goto fail;
			}
		}

fail:
	return r;
}

int init_list__exit(struct kvm *kvm)
{
	int i;
	int r = 0;
	struct hlist_node *n;
	struct init_item *t;

	for (i = ARRAY_SIZE(exit_lists) - 1; i >= 0; i--)
		hlist_for_each_entry(t, n, &exit_lists[i], n) {
			r = t->init(kvm);
			if (r < 0) {
				pr_warning("%s failed.\n", t->fn_name);
				goto fail;
			}
		}
fail:
	return r;
}
