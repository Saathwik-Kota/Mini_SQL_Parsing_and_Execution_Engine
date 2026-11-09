#ifndef MINI_SQL_DISK_MANAGER_H
#define MINI_SQL_DISK_MANAGER_H

#include "types.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    FILE* db_file;
    char file_path[256];
    page_id_t num_pages;
    uint64_t num_reads;
    uint64_t num_writes;
} disk_manager_t;

disk_manager_t* disk_manager_open(const char* file_path);
void disk_manager_close(disk_manager_t* dm);

bool disk_manager_read_page(disk_manager_t* dm, page_id_t page_id, void* page_data);
bool disk_manager_write_page(disk_manager_t* dm, page_id_t page_id, const void* page_data);
page_id_t disk_manager_allocate_page(disk_manager_t* dm);

page_id_t disk_manager_get_num_pages(const disk_manager_t* dm);
uint64_t disk_manager_get_num_reads(const disk_manager_t* dm);
uint64_t disk_manager_get_num_writes(const disk_manager_t* dm);
void disk_manager_reset_stats(disk_manager_t* dm);

#endif /* MINI_SQL_DISK_MANAGER_H */

