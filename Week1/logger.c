#include "logger.h"

/**
 * @function writeLog: Appends an activity log entry to the log file.
 *
 * @param choice: An integer representing the chosen function number (1-4).
 * @param value: A pointer to a string containing user input value.
 * @param result: A pointer to a string representing the result code (+OK or -ERR).
 *
 * @return: None.
 */
void writeLog(int choice, char *value, char *result) {
    FILE *f = fopen(LOG_FILE, "a");
    if (f == NULL) {
        return;
    }

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timeStr[25];
    strftime(timeStr, sizeof(timeStr), "%d/%m/%Y %H:%M:%S", t);

    if (value != NULL && strlen(value) > 0) {
        fprintf(f, "[%s] $ %d $ %s $ %s\n", timeStr, choice, value, result);
    } else {
        fprintf(f, "[%s] $ %d $ $ %s\n", timeStr, choice, result);
    }

    fclose(f);
}
