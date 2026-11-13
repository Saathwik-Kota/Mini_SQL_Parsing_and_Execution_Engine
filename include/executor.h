#ifndef MINI_SQL_EXECUTOR_H
#define MINI_SQL_EXECUTOR_H

#include "types.h"
#include "catalog.h"
#include "planner.h"
#include <stdbool.h>

typedef struct executor_t executor_t;

struct executor_t {
    void (*init)(executor_t* self);
    bool (*next)(executor_t* self, tuple_t* out_tuple);
    void (*close)(executor_t* self);
    void (*destroy)(executor_t* self);

    schema_t schema;
    void* state;
};

/* Executor Lifecycle */
executor_t* executor_create(catalog_t* catalog, const plan_node_t* plan);
void executor_destroy(executor_t* exec);

#endif /* MINI_SQL_EXECUTOR_H */

