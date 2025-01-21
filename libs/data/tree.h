#pragma once

#include <stddef.h>

#include "list.h"

// Nary tree
// This is _the worst_ way to implement this
typedef struct tree_node {
    struct tree_node* parent;
    void* data;

    linked_list* children;
} tree_node;

typedef struct tree {
    size_t nodes;
    tree_node* root;
} tree;


// Simple binary tree
typedef struct bin_tree_node {
    struct bin_tree_node* parent;

    void* data;

    struct bin_tree_node* left;
    struct bin_tree_node* right;
} bin_tree_node;

typedef struct bin_tree {
    size_t nodes;
    bin_tree_node* root;
} bin_tree;


// Red black tree
#define RBT_RED   ('R')
#define RBT_BLACK ('B')

typedef struct rb_tree_node {
    struct rb_tree_node* parent;

    char color;
    void* data;

    struct rb_tree_node* left;
    struct rb_tree_node* right;
} rb_tree_node;

typedef struct rb_tree {
    size_t nodes;
    rb_tree_node* root;
} rb_tree;

typedef bool (*tree_comp_fn)(const void* data, const void* private);
typedef bool (*tree_callback_fn)(tree_node* node);


tree* tree_create(void* root_data);
void tree_destroy(tree* root);

tree_node* tree_create_node(void* data);
void tree_destroy_node(tree_node* node);

void tree_prune(tree_node* parent);
void tree_prune_callback(tree_node* parent, tree_callback_fn callback);

void tree_insert_child(tree_node* parent, tree_node* child);
bool tree_remove_child(tree_node* parent, tree_node* child);

tree_node* tree_find_comp(tree* root, tree_comp_fn comp, void* private);
tree_node* tree_find(tree* root, void* data);
