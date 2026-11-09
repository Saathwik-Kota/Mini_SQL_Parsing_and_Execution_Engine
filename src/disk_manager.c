#include "../include/disk_manager.h"
#include <stdlib.h>
#include <string.h>

disk_manager_t* disk_manager_open(const char* file_path) {
    if (!file_path) {
        return NULL;
    }

    disk_manager_t* dm = (disk_manager_t*)calloc(1, sizeof(disk_manager_t));
    if (!dm) {
        return NULL;
    }

    strncpy(dm->file_path, file_path, sizeof(dm->file_path) - 1);

    /* Try to open existing database file */
    dm->db_file = fopen(file_path, "rb+");
    if (!dm->db_file) {
        /* Create new database file if it does not exist */
        dm->db_file = fopen(file_path, "wb+");
        if (!dm->db_file) {
            free(dm);
            return NULL;
        }
    }

    /* Compute number of existing pages based on file size */
    if (fseek(dm->db_file, 0, SEEK_END) == 0) {
        long file_size = ftell(dm->db_file);
        if (file_size > 0) {
            dm->num_pages = (page_id_t)(file_size / PAGE_SIZE);
        } else {
            dm->num_pages = 0;
        }
    } else {
        dm->num_pages = 0;
    }

    dm->num_reads = 0;
    dm->num_writes = 0;
    return dm;
}

void disk_manager_close(disk_manager_t* dm) {
    if (!dm) {
        return;
    }
    if (dm->db_file) {
        fflush(dm->db_file);
        fclose(dm->db_file);
        dm->db_file = NULL;
    }
    free(dm);
}

bool disk_manager_read_page(disk_manager_t* dm, page_id_t page_id, void* page_data) {
    if (!dm || !dm->db_file || !page_data) {
        return false;
    }

    long offset = (long)page_id * PAGE_SIZE;
    if (fseek(dm->db_file, offset, SEEK_SET) != 0) {
        return false;
    }

    size_t bytes_read = fread(page_data, 1, PAGE_SIZE, dm->db_file);
    if (bytes_read < PAGE_SIZE) {
        /* Clear remaining buffer if short read (e.g. newly allocated unwritten block) */
        memset((uint8_t*)page_data + bytes_read, 0, PAGE_SIZE - bytes_read);
    }

    dm->num_reads++;
    return true;
}

bool disk_manager_write_page(disk_manager_t* dm, page_id_t page_id, const void* page_data) {
    if (!dm || !dm->db_file || !page_data) {
        return false;
    }

    long offset = (long)page_id * PAGE_SIZE;
    if (fseek(dm->db_file, offset, SEEK_SET) != 0) {
        return false;
    }

    size_t bytes_written = fwrite(page_data, 1, PAGE_SIZE, dm->db_file);
    if (bytes_written != PAGE_SIZE) {
        return false;
    }

    fflush(dm->db_file);
    dm->num_writes++;

    if (page_id >= dm->num_pages) {
        dm->num_pages = page_id + 1;
    }

    return true;
}

page_id_t disk_manager_allocate_page(disk_manager_t* dm) {
    if (!dm) {
        return INVALID_PAGE_ID;
    }

    page_id_t new_page_id = dm->num_pages;
    uint8_t zero_page[PAGE_SIZE];
    memset(zero_page, 0, PAGE_SIZE);

    if (!disk_manager_write_page(dm, new_page_id, zero_page)) {
        return INVALID_PAGE_ID;
    }

    return new_page_id;
}

page_id_t disk_manager_get_num_pages(const disk_manager_t* dm) {
    return dm ? dm->num_pages : 0;
}

uint64_t disk_manager_get_num_reads(const disk_manager_t* dm) {
    return dm ? dm->num_reads : 0;
}

uint64_t disk_manager_get_num_writes(const disk_manager_t* dm) {
    return dm ? dm->num_writes : 0;
}

void disk_manager_reset_stats(disk_manager_t* dm) {
    if (dm) {
        dm->num_reads = 0;
        dm->num_writes = 0;
    }
}

