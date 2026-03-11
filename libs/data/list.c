#include "list.h"

#include <stdlib.h>
#include <string.h>

#if defined(DEBUG) || defined(LIST_VALIDATE)
#define LIST_VALIDATE_NODES 1
#else
#define LIST_VALIDATE_NODES 0
#endif

#if LIST_VALIDATE_NODES
static bool list_contains_node(const linked_list_t *list, const list_node_t *node) {
    if (!list || !node) {
        return false;
    }

    for (list_node_t *it = list->head; it; it = it->next) {
        if (it == node) {
            return true;
        }
    }

    return false;
}
#endif

linked_list_t *list_create(void) {
    linked_list_t *new = calloc(1, sizeof(linked_list_t));
    if (!new) {
        return NULL;
    }

    new->length = 0;
    new->head = NULL;
    new->tail = NULL;

    return new;
}

void list_destroy(linked_list_t *list, bool free_data) {
    if (!list) {
        return;
    }

    list_node_t *node = list->head;
    while (node) {
        list_node_t *next = node->next;

        if (free_data && node->data) {
            free(node->data);
        }

        free(node);
        node = next;
    }

    free(list);
}


list_node_t *list_create_node(void *data) {
    list_node_t *new = calloc(1, sizeof(list_node_t));
    if (!new) {
        return NULL;
    }

    new->next = NULL;
    new->prev = NULL;
    new->owner = NULL;
    new->data = data;

    return new;
}

void list_destroy_node(list_node_t *node) {
    free(node);
}


bool list_append(linked_list_t *list, list_node_t *node) {
    if (!node || !list) {
        return false;
    }

#if LIST_VALIDATE_NODES
    if (list_contains_node(list, node)) {
        return false;
    }
#endif
    if (node->owner) {
        return false;
    }
    if (node->next || node->prev || list->head == node || list->tail == node) {
        return false;
    }

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

    node->owner = list;
    list->length++;

    return true;
}

bool list_remove(linked_list_t *list, list_node_t *node) {
    if (!node || !list || !list->length) {
        return false;
    }

    list_node_t *prev = node->prev;
    list_node_t *next = node->next;
    bool linked_consistent = (
        node->owner == list &&
        (!prev || prev->next == node) &&
        (!next || next->prev == node) &&
        (node == list->head || prev) &&
        (node == list->tail || next)
    );

    if (!linked_consistent) {
        prev = NULL;
        next = list->head;
        size_t limit = list->length ? list->length : 1;

        while (next && next != node && limit--) {
            prev = next;
            next = next->next;
        }

        if (next != node) {
            return false;
        }

        next = node->next;
    }

#if LIST_VALIDATE_NODES
    if (!list_contains_node(list, node)) {
        return false;
    }
#endif

    if (prev) {
        prev->next = next;
    } else {
        list->head = next;
    }

    if (next) {
        next->prev = prev;
    } else {
        list->tail = prev;
    }

    node->prev = NULL;
    node->next = NULL;
    node->owner = NULL;

    list->length--;

    return true;
}


bool list_swap(list_node_t *left, list_node_t *right) {
    if (!left || !right) {
        return false;
    }

    if (left->prev) {
        left->prev->next = right;
    }
    if (right->prev) {
        right->prev->next = left;
    }

    if (left->next) {
        left->next->prev = right;
    }
    if (right->next) {
        right->next->prev = left;
    }

    memswap(&left->prev, &right->prev, sizeof(list_node_t *));
    memswap(&left->next, &right->next, sizeof(list_node_t *));

    return true;
}


bool list_push(linked_list_t *list, list_node_t *node) {
    if (!node || !list) {
        return false;
    }
    if (node->owner) {
        return false;
    }
    if (node->next || node->prev || list->head == node || list->tail == node) {
        return false;
    }

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

    node->owner = list;
    list->length++;

    return true;
}

list_node_t *list_pop(linked_list_t *list) {
    if (!list || !list->length) {
        return NULL;
    }

    list_node_t *tail = list->tail;
    if (!list_remove(list, tail)) {
        return NULL;
    }

    return tail;
}

list_node_t *list_pop_front(linked_list_t *list) {
    if (!list || !list->length) {
        return NULL;
    }

    list_node_t *head = list->head;
    if (!list_remove(list, head)) {
        return NULL;
    }

    return head;
}


list_node_t *list_find(linked_list_t *list, void *data) {
    ll_foreach(node, list) {
        if (node->data == data) {
            return node;
        }
    }

    return NULL;
}
