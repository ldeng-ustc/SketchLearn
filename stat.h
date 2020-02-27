#ifndef __STAT_H__
#define __STAT_H__

#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef struct _stat_t
{
    uint64_t id;
    uint64_t time_load;
    uint64_t time_infer;
}stat_t;

void stat_print(stat_t *stat, FILE *fp) {
    fprintf(fp, "Stat %lu:\n\tload(us): %lu\n\tinfer(us): %lu\n", stat->id, stat->time_load, stat->time_infer);
}

#endif