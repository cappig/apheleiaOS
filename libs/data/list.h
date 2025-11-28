#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct list_node {
    void* data;

    struct list_node* next;
    struct list_node* prev;
} list_node_t;

typedef struct linked_list {
    size_t length;

    list_node_t* head;
    list_node_t* tail;
} linked_list_t;

#define ll_foreach(node, list) \
    for (list_node_t* node = (list)->head; node != NULL; node = node->next)

#define ll_foreach_from(node, from) \
    for (list_node_t* node = (from); node != NULL; node = node->next)


linked_list_t* list_create(void);
void list_destroy(linked_list_t* list, bool free_data);

list_node_t* list_create_node(void* data);
void list_destroy_node(list_node_t* node);

bool list_append(linked_list_t* list, list_node_t* node);
bool list_remove(linked_list_t* list, list_node_t* node);

bool list_swap(list_node_t* left, list_node_t* right);

bool list_push(linked_list_t* list, list_node_t* node);
list_node_t* list_pop(linked_list_t* list);
list_node_t* list_pop_front(linked_list_t* list);

list_node_t* list_find(linked_list_t* list, void* data);
