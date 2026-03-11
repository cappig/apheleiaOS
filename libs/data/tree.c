#include "tree.h"

#include <stdlib.h>

#include "list.h"


tree_node_t *tree_create_node(void *data) {
    tree_node_t *new = calloc(1, sizeof(tree_node_t));

    new->children = list_create();
    new->parent = NULL;
    new->data = data;

    return new;
}

void tree_destroy_node(tree_node_t *node) {
    if (node->children) {
        list_destroy(node->children, false);
    }

    free(node);
}


tree_t *tree_create_rooted(tree_node_t *root) {
    tree_t *new = malloc(sizeof(tree_t));
    new->root = root;
    new->nodes = 1;

    return new;
}

tree_t *tree_create(void *root_data) {
    tree_node_t *root = tree_create_node(root_data);
    return tree_create_rooted(root);
}

void tree_destroy(tree_t *trunk) {
    if (!trunk) {
        return;
    }

    tree_prune(trunk->root);
    free(trunk);
}


void tree_prune_callback(tree_node_t *parent, tree_callback_fn callback) {
    if (!parent) {
        return;
    }

    if (parent->children) {
        ll_foreach(node, parent->children) {
            tree_node_t *child = node->data;
            tree_prune_callback(child, callback);
        }

        list_destroy(parent->children, false);
        parent->children = NULL;
    }

    if (callback) {
        callback(parent);
    }

    free(parent);
}

void tree_prune(tree_node_t *parent) {
    tree_prune_callback(parent, NULL);
}


bool tree_insert_child(tree_node_t *parent, tree_node_t *child) {
    if (!parent || !child) {
        return false;
    }

    if (!parent->children) {
        parent->children = list_create();
    }

    if (!parent->children) {
        return false;
    }

    list_node_t *node = list_create_node(child);
    if (!node || !list_append(parent->children, node)) {
        list_destroy_node(node);
        return false;
    }

    child->parent = parent;

    return true;
}

bool tree_remove_child(tree_node_t *parent, tree_node_t *child) {
    if (!parent || !child || !parent->children) {
        return false;
    }

    linked_list_t *list = parent->children;

    list_node_t *lnode = list_find(list, child);

    if (!lnode) {
        return false;
    }

    bool removed = list_remove(list, lnode);

    list_destroy_node(lnode);

    return removed;
}

static tree_node_t *
_findc(tree_node_t *root, tree_comp_fn comp, void *private) {
    ll_foreach(node, root->children) {
        tree_node_t *child = node->data;

        if (comp(child->data, private)) {
            return child;
        }

        tree_node_t *res = _findc(child, comp, private);

        if (res) {
            return res;
        }
    }

    return NULL;
}

tree_node_t *tree_find_comp(tree_t *root, tree_comp_fn comp, void *private) {
    if (!root) {
        return NULL;
    }

    tree_node_t *rnode = root->root;

    if (comp(rnode->data, private)) {
        return rnode;
    }

    return _findc(root->root, comp, private);
}

static bool _find_comp(const void *data, void *private) {
    return (data == private);
}

tree_node_t *tree_find(tree_t *root, void *data) {
    if (!root) {
        return NULL;
    }

    return tree_find_comp(root, _find_comp, data);
}


int tree_foreach_node(
    tree_node_t *node,
    tree_foreach_fn callback,
    void *private
) {
    if (!node || !callback) {
        return -1;
    }

    // callback requested early termination
    if (callback(node->data, private)) {
        return 1;
    }

    ll_foreach(lnode, node->children) {
        tree_node_t *child = lnode->data;

        if (tree_foreach_node(child, callback, private)) {
            return 1; // early termination from child
        }
    }

    return 0;
}

int tree_foreach(tree_t *root, tree_foreach_fn callback, void *data) {
    if (!root || !root->root) {
        return -1;
    }

    return tree_foreach_node(root->root, callback, data);
}
