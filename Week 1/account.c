#include "account.h"

/**
 * @function pushToList: Adds a new account to the linked list.
 *
 * @param root: A pointer to the pointer of head node.
 * @param acc: An account structure containing username and status.
 *
 * @return: None.
 */
void pushToList(node **root, account acc) {
    node *newNode = (node*)malloc(sizeof(node));
    if (newNode == NULL) {
        printf("Memory allocation failed!\n");
        return;
    }
    newNode->acc = acc;
    newNode->next = *root;
    *root = newNode;
}

/**
 * @function findAccount: Searches for an account by username in the linked list.
 *
 * @param root: A pointer to head node of the linked list.
 * @param username: A pointer to a string containing the username to search.
 *
 * @return: A pointer to the matched node if found, NULL otherwise.
 */
node* findAccount(node *root, char *username) {
    node *cur = root;
    while (cur != NULL) {
        if (strcmp(cur->acc.username, username) == 0) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

/**
 * @function freeList: Deallocates all memory occupied by the linked list.
 *
 * @param head: A pointer to head node of the linked list.
 *
 * @return: None.
 */
void freeList(node *head) {
    node *tmp;
    while (head != NULL) {
        tmp = head;
        head = head->next;
        free(tmp);
    }
}
