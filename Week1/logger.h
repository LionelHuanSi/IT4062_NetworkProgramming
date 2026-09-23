#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <time.h>
#include <string.h>

#define LOG_FILE "log_20235733.txt"

/**
 * @function writeLog: Appends an activity log entry to the log file.
 *
 * @param choice: An integer representing the chosen function number (1-4).
 * @param value: A pointer to a string containing user input value.
 * @param result: A pointer to a string representing the result code (+OK or -ERR).
 *
 * @return: None.
 */
void writeLog(int choice, char *value, char *result);

#endif
