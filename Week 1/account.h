#ifndef ACCOUNT_H
#define ACCOUNT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACTIVE 1
#define BANNED 0

typedef struct account {
    char username[50];
    int status;
} account;

typedef struct node {
    account acc;
    struct node *next;
} node;

/**
 * @function pushToList: Adds a new account to the linked list.
 *
 * @param root: A pointer to the pointer of head node.
 * @param acc: An account structure containing username and status.
 *
 * @return: None.
 */
void pushToList(node **root, account acc);

/**
 * @function findAccount: Searches for an account by username in the linked list.
 *
 * @param root: A pointer to head node of the linked list.
 * @param username: A pointer to a string containing the username to search.
 *
 * @return: A pointer to the matched node if found, NULL otherwise.
 */
node* findAccount(node *root, char *username);

/**
 * @function freeList: Deallocates all memory occupied by the linked list.
 *
 * @param head: A pointer to head node of the linked list.
 *
 * @return: None.
 */
void freeList(node *head);

#endif
