#include "tree.h"

#include <alloc/global.h>

#include "list.h"


tree_node* tree_create_node(void* data) {
    tree_node* new = gcalloc(sizeof(tree_node));
    new->children = list_create();
    new->parent = NULL;
    new->data = data;

    return new;
}

void tree_destroy_node(tree_node* node) {
    gfree(node);
}

tree* tree_create(void* root_data) {
    tree* new = gmalloc(sizeof(tree));
    new->root = tree_create_node(root_data);
    new->nodes = 1;

    return new;
}

void tree_destroy(tree* root) {
    tree_prune(root->root);
    gfree(root);
}

// TODO: implement a simple callback function that can free the data
void tree_prune(tree_node* parent) {
    foreach (node, parent->children) {
        tree_node* child = node->data;
        tree_prune(child);
        gfree(child);
    }

    list_destroy(parent->children);
    gfree(parent);
}

void tree_insert_child(tree_node* parent, tree_node* child) {
    list_append(parent->children, list_create_node(child));
    child->parent = parent;
}


static tree_node* _findc(tree_node* root, tree_comp_fn comp, void* private) {
    foreach (node, root->children) {
        tree_node* child = node->data;

        if (comp(child->data, private))
            return child;

        tree_node* res = _findc(child, comp, private);
        if (res)
            return res;
    }

    return NULL;
}

tree_node* tree_find_comp(tree* root, tree_comp_fn comp, void* private) {
    if (!root)
        return NULL;

    tree_node* rnode = root->root;
    if (comp(rnode->data, private))
        return rnode;

    return _findc(root->root, comp, private);
}

static bool _find_comp(const void* data, const void* private) {
    return (data == private);
}

tree_node* tree_find(tree* root, void* data) {
    if (!root)
        return NULL;

    return tree_find_comp(root, _find_comp, data);
}
