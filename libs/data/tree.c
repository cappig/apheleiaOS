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

tree* tree_create(void* root_data) {
    tree* new = gmalloc(sizeof(tree));
    new->root = tree_create_node(root_data);
    new->nodes = 1;

    return new;
}

void tree_insert_child(tree_node* parent, tree_node* child) {
    list_append(parent->children, list_create_node(child));
    child->parent = parent;
}
