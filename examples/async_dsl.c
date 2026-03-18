#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Mock future/future_ready/future_state ---
typedef enum { FUTURE_PENDING, FUTURE_RESOLVED, FUTURE_REJECTED } future_state_t;

typedef struct future {
    future_state_t state;
    void *result;
    int error_code;
} future_t;

// Mock helpers
int future_ready(future_t *fut) { return fut->state != FUTURE_PENDING; }

// --- DSL macros ---
#define ASYNC(name) void name()
#define DONE

#define UNIQUE_ERROR_LABEL(name) ERROR_##name
#define AWAIT(fut, err) \
    do { \
        while (!future_ready(fut)) return; \
        if ((fut)->state == FUTURE_REJECTED) { \
            err = (fut)->error_code; \
            goto UNIQUE_ERROR_LABEL(__func__); \
        } \
    } while(0)

#define CATCH(err) UNIQUE_ERROR_LABEL(__func__): if (err)

// --- Example async workflow ---
ASYNC(download_process_save)
{
    int err = 0;

    // Step 1: Download file
    future_t download_future = { FUTURE_RESOLVED, "file content", 0 }; // mock success
    AWAIT(&download_future, err);
    printf("Downloaded file: %s\n", (char *)download_future.result);

    // Step 2: Process file
    future_t process_future = { FUTURE_REJECTED, NULL, 42 }; // mock failure
    AWAIT(&process_future, err);
    printf("Processed file successfully!\n");

    // Step 3: Save file (would not reach here if previous failed)
    future_t save_future = { FUTURE_RESOLVED, NULL, 0 };
    AWAIT(&save_future, err);
    printf("File saved successfully!\n");

    return;

    // Error handling block
    CATCH(err) {
        printf("Operation failed, error code: %d\n", err);
        // additional recovery or logging can go here
    }
}
