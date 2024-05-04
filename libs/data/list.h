#pragma once

#include <stddef.h>

#define foreach(node, list) for (list_node* node = (list)->head; node != NULL; node = node->next)

#define foreach_from(node, from) for (list_node* node = (from); node != NULL; node = node->next)


typedef struct list_node {
    void* data;

    struct list_node* next;
    struct list_node* prev;
} list_node;

typedef struct linked_list {
    size_t length;

    list_node* head;
    list_node* tail;
} linked_list;


linked_list* list_create(void);
void list_destroy(linked_list* list);

list_node* list_create_node(void* data);
void list_destory_node(list_node* node);

void list_append(linked_list* list, list_node* node);
void list_remove(linked_list* list, list_node* node);

void list_queue_swap(linked_list* list);

list_node* list_find(linked_list* list, void* data);
