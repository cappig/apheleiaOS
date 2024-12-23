#include "list.h"

#include <alloc/global.h>
#include <string.h>


linked_list* list_create(void) {
    linked_list* new = gcalloc(sizeof(linked_list));
    new->length = 0;
    new->head = NULL;
    new->tail = NULL;

    return new;
}

// NOTE: this function does not free the stored data!
void list_destroy(linked_list* list) {
    foreach (node, list) {
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

void list_destroy_node(list_node* node) {
    gfree(node);
}

bool list_append(linked_list* list, list_node* node) {
    if (!node || !list)
        return false;

    node->next = NULL;

    if (!list->length) {
        node->prev = NULL;
        list->head = node;
        list->tail = node;
    } else {
        list->tail->next = node;
        node->prev = list->tail;
        list->tail = node;
    }

    list->length++;

    return true;
}

bool list_remove(linked_list* list, list_node* node) {
    if (!node || !list)
        return false;

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

    return true;
}

bool list_swap(list_node* left, list_node* right) {
    if (!left || !right)
        return false;

    if (left->prev)
        left->prev->next = right;
    if (right->prev)
        right->prev->next = left;

    if (left->next)
        left->next->prev = right;
    if (right->next)
        right->next->prev = left;

    memswap(&left->prev, &right->prev, sizeof(list_node*));
    memswap(&left->next, &right->next, sizeof(list_node*));

    return true;
}

bool list_push(linked_list* list, list_node* node) {
    if (!node || !list)
        return false;

    node->prev = NULL;

    if (!list->length) {
        node->next = NULL;
        list->head = node;
        list->tail = node;
    } else {
        list->head->prev = node;
        node->next = list->head;
        list->head = node;
    }

    list->length++;

    return true;
}

list_node* list_pop(linked_list* list) {
    if (!list->length)
        return NULL;

    list_node* tail = list->tail;
    list_remove(list, tail);

    return tail;
}

list_node* list_pop_front(linked_list* list) {
    if (!list->length)
        return NULL;

    list_node* head = list->head;
    list_remove(list, head);

    return head;
}

list_node* list_find(linked_list* list, void* data) {
    foreach (node, list) {
        if (node->data == data)
            return node;
    }

    return NULL;
}
