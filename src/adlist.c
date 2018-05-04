#include <stdio.h>
#include "adlist.h"
#include "zmalloc.h"

list *listCreate(void) {
    list *list;

    if ((list = zmalloc(sizeof(*list))) == NULL) return NULL;
    list->head = list->tail = NULL;
    list->len = 0;
    list->dup = NULL;
    list->free = NULL;
    list->match = NULL;
    return list;
}

/* Remove all the elements from the list without destroying the list itself. */
void listEmpty(list *list) {
    listNode *current, *next;

    current = list->head;
    while(current) {
        next = current->next;
        if (list->free) list->free(current->value);
        list->head = current->next;
        zfree(current);
        current = next;
    }
    list->head = list->tail = NULL;
    list->len = 0;
}

void listRelease(list *list) {
    listEmpty(list);
    zfree(list);
}

list *listAddNodeHead(list *list, void* value) {
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL) return NULL;
    node->value = value;
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        node->prev = NULL;
        node->next = list->head;
        list->head->prev = node;
        list->head = node;
    }
    list->len++;
    return list;
}

list *listAddNodeTail(list *list, void* value) {
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL) return NULL;
    node->value = value;
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {
        node->next = NULL;
        node->prev = list->tail;
        list->tail->next = node;
        list->tail = node;
    }
    list->len++;
    return list;
}

list *listInsertNode(list *list, listNode *old_value, void *value, int after) {
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL) return NULL;
    node->value = value;
    if (after) {
        node->prev = old_value;
        node->next = old_value->next;
        if (list->tail == old_value) {
            list->tail = node;
        }
    } else {
        node->next = old_value;
        node->prev = old_value->prev;
        if (list->head == old_value) {
            list->head = node;
        }
    }
    if (node->prev != NULL) {
        node->prev->next = node;
    }
    if (node->next != NULL) {
        node->next->prev = node;
    }
    return list;
}

void listDelNode(list *list, listNode *node) {
    if (node->prev)
        node->prev->next = node->next;
    else
        list->head = node->next;
    if (node->next)
        node->next->prev = node->prev;
    else
        list->tail = node->prev;
    if (list->free) list->free(node->value);
    zfree(node);
    list->len--;
}

list* listDup(list *orig) {
    list *copy;
    listNode *node;

    if ((copy = listCreate()) == NULL) return NULL;
    copy->dup = orig->dup;
    copy->free = orig->free;
    copy->match = orig->match;
    node = orig->head;
    while(node != NULL) {
        void *value;

        if (copy->dup) {
            value = copy->dup(node->value);
            if (value == NULL) {
                listRelease(copy);
                return NULL;
            }
        } else
            value = node->value;
        if (listAddNodeHead(copy, value) == NULL) {
            listRelease(copy);
            return NULL;
        }
        node = node->next;
    }
    return copy;
}

/* Rotate the list removing the tail node and inserting it to the head. */
void listRotate(list *list) {
    listNode *tail = list->tail;

    if (listLength(list) <= 1) return;

    /* Detach current tail */
    list->tail = tail->prev;
    list->tail->next = NULL;
    /* Move it as head */
    list->head->prev = tail;
    tail->prev = NULL;
    tail->next = list->head;
    list->head = tail;
}
