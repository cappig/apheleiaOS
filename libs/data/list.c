#include "list.h"

#include <stdlib.h>
#include <string.h>


linked_list_t* list_create(void) {
    linked_list_t* new = calloc(1, sizeof(linked_list_t));
    new->length = 0;
    new->head = NULL;
    new->tail = NULL;

    return new;
}

void list_destroy(linked_list_t* list, bool free_data) {
    if (!list)
        return;

    list_node_t* node = list->head;
    while (node) {
        list_node_t* next = node->next;

        if (free_data && node->data)
            free(node->data);

        free(node);
        node = next;
    }

    free(list);
}


list_node_t* list_create_node(void* data) {
    list_node_t* new = calloc(1, sizeof(list_node_t));
    new->next = NULL;
    new->prev = NULL;
    new->data = data;

    return new;
}

void list_destroy_node(list_node_t* node) {
    free(node);
}


bool list_append(linked_list_t* list, list_node_t* node) {
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

bool list_remove(linked_list_t* list, list_node_t* node) {
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


bool list_swap(list_node_t* left, list_node_t* right) {
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

    memswap(&left->prev, &right->prev, sizeof(list_node_t*));
    memswap(&left->next, &right->next, sizeof(list_node_t*));

    return true;
}


bool list_push(linked_list_t* list, list_node_t* node) {
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

list_node_t* list_pop(linked_list_t* list) {
    if (!list->length)
        return NULL;

    list_node_t* tail = list->tail;
    list_remove(list, tail);

    return tail;
}

list_node_t* list_pop_front(linked_list_t* list) {
    if (!list->length)
        return NULL;

    list_node_t* head = list->head;
    list_remove(list, head);

    return head;
}


list_node_t* list_find(linked_list_t* list, void* data) {
    ll_foreach(node, list) {
        if (node->data == data)
            return node;
    }

    return NULL;
}
