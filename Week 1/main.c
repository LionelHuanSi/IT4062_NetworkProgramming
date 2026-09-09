#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "account.h"
#include "logger.h"

char currentUser[50] = "";

/**
 * @function readFile: Reads accounts from account.txt and loads them into a linked list.
 *
 * @param root: A pointer to the pointer of head node.
 *
 * @return: None.
 */
void readFile(node **root) {
    FILE *f = fopen("account.txt", "r");
    if (f == NULL) {
        printf("Cannot open account.txt!\n");
        return;
    }

    account acc;
    while (fscanf(f, "%s %d", acc.username, &acc.status) == 2) {
        pushToList(root, acc);
    }
    fclose(f);
}

/**
 * @function login: Handles user login authentication.
 *
 * @param root: A pointer to head node of the account linked list.
 *
 * @return: None.
 */
void login(node *root) {
    char name[50];
    printf("Username: ");
    scanf("%s", name);

    if (strlen(currentUser) > 0) {
        printf("You have already logged in\n");
        writeLog(1, name, "-ERR");
        return;
    }

    node *found = findAccount(root, name);
    if (found == NULL) {
        printf("Account is not exist\n");
        writeLog(1, name, "-ERR");
    } else if (found->acc.status == BANNED) {
        printf("Account is banned\n");
        writeLog(1, name, "-ERR");
    } else {
        printf("Hello %s\n", name);
        strcpy(currentUser, name);
        writeLog(1, name, "+OK");
    }
}

/**
 * @function postMessage: Handles posting a message for logged-in user.
 *
 * @return: None.
 */
void postMessage() {
    char message[256];
    printf("Post message: ");
    scanf(" %[^\n]", message);

    if (strlen(currentUser) == 0) {
        printf("You have not logged in.\n");
        writeLog(2, message, "-ERR");
    } else {
        printf("Successful post\n");
        writeLog(2, message, "+OK");
    }
}

/**
 * @function logout: Logs out the currently active user.
 *
 * @return: None.
 */
void logout() {
    if (strlen(currentUser) == 0) {
        printf("You have not logged in.\n");
        writeLog(3, "", "-ERR");
    } else {
        currentUser[0] = '\0';
        printf("Successful log out\n");
        writeLog(3, "", "+OK");
    }
}

int main() {
    node *root = NULL;
    readFile(&root);

    int choice;
    do {
        printf("1. Log in\n");
        printf("2. Post message\n");
        printf("3. Logout\n");
        printf("4. Exit\n");
        printf("Your choice: ");

        if (scanf("%d", &choice) != 1) {
            break;
        }

        switch (choice) {
            case 1:
                login(root);
                break;
            case 2:
                postMessage();
                break;
            case 3:
                logout();
                break;
            case 4:
                writeLog(4, "", "+OK");
                break;
            default:
                printf("Invalid choice!\n");
                break;
        }
        printf("\n");
    } while (choice != 4);

    freeList(root);
    return 0;
}
