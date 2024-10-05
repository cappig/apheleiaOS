#pragma once

#include <stddef.h>

#include "list.h"

// Nary tree
// This is _the worst_ way to implement this
// TODO: we should use a c++ style vector instead
typedef struct tree_node {
    void* data;

    struct tree_node* parent;
    struct linked_list* children;
} tree_node;

typedef struct tree {
    size_t nodes;
    tree_node* root;
} tree;


// Simple binary tree
typedef struct bin_tree_node {
    void* data;

    struct bin_tree_node* parent;
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
    void* data;

    char color;

    struct rb_tree_node* parent;

    struct rb_tree_node* left;
    struct rb_tree_node* right;
} rb_tree_node;

typedef struct rb_tree {
    size_t nodes;

    rb_tree_node* root;
} rb_tree;


tree* tree_create(void* root_data);
void tree_destory(tree* root);

tree_node* tree_create_node(void* data);
void tree_destory_node(tree_node* node);

void tree_prune(tree_node* parent);

void tree_insert_child(tree_node* parent, tree_node* child);
