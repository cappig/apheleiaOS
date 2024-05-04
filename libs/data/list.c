#include "list.h"

#include <alloc/global.h>


linked_list* list_create(void) {
    linked_list* new = gcalloc(sizeof(linked_list));
    new->length = 0;
    new->head = NULL;
    new->tail = NULL;

    return new;
}

void list_destroy(linked_list* list) {
    list_node* node = list->head;
    list_node* next = node->next;

    while (next) {
        next = node->next;
        gfree(node);
    }

    gfree(list);
}

list_node* list_create_node(void* data) {
    list_node* new = gcalloc(sizeof(list_node));
    new->next = NULL;
    new->prev = NULL;
    new->data = data;

    return new;
}

void list_destory_node(list_node* node) {
    gfree(node);
}

void list_append(linked_list* list, list_node* node) {
    node->next = NULL;

    if (list->length == 0) {
        node->prev = NULL;
        list->head = node;
        list->tail = node;
    } else {
        list->tail->next = node;
        node->prev = list->tail;
        list->tail = node;
    }

    list->length++;
}

void list_remove(linked_list* list, list_node* node) {
    if (node == list->head)
        list->head = node->next;
    if (node == list->tail)
        list->tail = node->prev;

    if (node->prev)
        node->prev->next = node->next;
    if (node->next)
        node->next->prev = node->prev;

    node->prev = NULL;
    node->next = NULL;

    list->length--;
}

// The current head of the list becomes the new tail
void list_queue_swap(linked_list* list) {
    if (list->length <= 1)
        return;

    list_node* current_head = list->head;

    list->head = list->head->next;
    current_head->next = NULL;

    list->tail->next = current_head;
    list->tail = current_head;
}

list_node* list_find(linked_list* list, void* data) {
    foreach (node, list) {
        if (node->data == data)
            return node;
    }

    return NULL;
}
