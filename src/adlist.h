/* adlist.h - A generic doubly linked list implementation */

#ifndef __ADLIST_H__
#define __ADLIST_H__

/* Node, List, and Iterator are the only data structures used currently. */

typedef struct listNode {
    struct listNode *prev;
    struct listNode *next;
    void *value;
} listNode;

typedef struct list {
    struct listNode *head;
    struct listNode *tail;
    void *(*dup)(void *ptr);
    void (*free)(void *ptr);
    int (*match)(void *ptr, void *key);
    unsigned long len;
} list;

#define listNodePrev(n) ((n)->prev)
#define listNodeNext(n) ((n)->next)
#define listNodeValue(n) ((n)->value)
#define listLength(l) ((l)->len)
#define listFirst(l) ((l)->head)
#define listLast(l) ((l)->tail)

#define listSetDupMethod(l,m) ((l)->dup = (m))
#define listSetFreeMethod(l,m) ((l)->free = (m))
#define listSetMatchMethod(l,m) ((l)->match = (m))

#define listGetDupMethod(l) ((l)->dup)
#define listGetFree(l) ((l)->free)
#define listGetMatchMethod(l) ((l)->match)

/* Prototypes */
list *listCreate(void);
void listEmpty(list *list);
void listRelease(list *list);
list *listAddNodeHead(list *list, void* value);
list *listAddNodeTail(list *list, void* value);
list *listInsertNode(list *list, listNode *old_value, void *value, int after);
void listDelNode(list *list, listNode *node);
list* listDup(list *list);
void listRotate(list *list);

#endif /* __ADLIST_H__ */
