#pragma once

#include <stddef.h>

#include "list.h"

// n-ary tree
typedef struct tree_node {
    struct tree_node* parent;
    void* data;

    linked_list_t* children;
} tree_node_t;

typedef struct tree {
    size_t nodes;
    tree_node_t* root;
} tree_t;


typedef bool (*tree_comp_fn)(const void* data, void* private);
typedef bool (*tree_callback_fn)(tree_node_t* node);
typedef bool (*tree_foreach_fn)(const void* data, void* private);

tree_t* tree_create_rooted(tree_node_t* root);
tree_t* tree_create(void* root_data);
void tree_destroy(tree_t* root);

tree_node_t* tree_create_node(void* data);
void tree_destroy_node(tree_node_t* node);

void tree_prune(tree_node_t* parent);
void tree_prune_callback(tree_node_t* parent, tree_callback_fn callback);

bool tree_insert_child(tree_node_t* parent, tree_node_t* child);
bool tree_remove_child(tree_node_t* parent, tree_node_t* child);

tree_node_t* tree_find_comp(tree_t* root, tree_comp_fn comp, void* private);
tree_node_t* tree_find(tree_t* root, void* data);

int tree_foreach_node(tree_node_t* node, tree_foreach_fn callback, void* data);
int tree_foreach(tree_t* tree, tree_foreach_fn callback, void* data);
