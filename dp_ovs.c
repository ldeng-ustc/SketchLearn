#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>
#include <errno.h>

#include <semaphore.h>

#include "util.h"
#include "ringbuffer.h"
#include "packet.h"
#include "alg_sketchlearn.h"
#include "config.h"
#include "cpu.h"

#include "hash.h"
#include "alg_keytbl.h"

#include "adapter_record_ram.h"

extern conf_t* conf;
extern struct PacketStat packet_stat;

const char* alg = NULL;
int is_output = 0;
int is_pin_cpu = 0;

typedef struct Info {
    int id;
    ringbuffer_t* rb;

    pthread_t thread;
    sem_t sem;
} info_t;

ringbuffer_t* krb;

static void* thread_handler(void *arg) {
    info_t* args = (info_t*)arg;
    ringbuffer_t* rb = args->rb;

    // Modity: init
    int width = conf_skl_width(conf);
    int depth = conf_skl_depth(conf);
    int key_len = conf_common_key_len(conf);
    SketchLearn_t* skl = SKL_Init(width, depth, key_len);
    width = skl->width;
    depth = skl->depth;

    char tmp[100];

    tuple_t t;
    memset(&t, 0, sizeof(struct Tuple));
    unsigned long long cnt = 0;
    unsigned long long cur_interval = 0;

    unsigned long long tot_byte = 0;

    if (is_pin_cpu) {
        pin_thread(args->id);
    }
    sem_post(&args->sem);
    while (1) {
	    while (read_ringbuffer(rb, &t) < 0) {}

        if (t.flag == TUPLE_TERM) {
            break;
        }
        else if (t.flag == TUPLE_PUNC) {

            // Modity: output and reset
            if (is_output) {
                SKL_CompleteZeroes(skl);
                sprintf(tmp, "%s/sketches_ovs/%s_%llu",
                        conf_common_trace_dir(conf), alg, cur_interval);
                SKL_Print(skl, tmp);
            }

            cur_interval++;
            tot_byte = 0;
            SKL_Reset(skl);
            sem_post(&args->sem);
            continue;
        }

        // Modity: update
        SKL_Update_Fast(skl, (unsigned char*)&t.key, t.size);
        cnt++;
        tot_byte += t.size;
    }

    // Modity: output and free
    if (is_output) {
        SKL_CompleteZeroes(skl);
        char tmp[100];
        sprintf(tmp, "%s/sketches/%s_%llu",
                conf_common_trace_dir(conf), alg, cur_interval);
        SKL_Print(skl, tmp);
    }

    SKL_Destroy(skl);
    
    pthread_exit((void*)cnt);
}

int main (int argc, char *argv []) {

    int num_thread = 0;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s [config file] [num of threads]\n", argv[0]);
        exit(-1);
    }
        
    conf = Config_Init(argv[1]);
    num_thread = atoi(argv[2]);

    char tmp[100];
    sprintf(tmp, "%s/sketches_ovs", conf_common_trace_dir(conf));
    mkdir(tmp, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    krb = connect_ringbuffer_kshm("/dev/sketchvisor", sizeof(tuple_t));

    alg = "sketchlearn";
    is_output = conf_common_is_output(conf);
    is_pin_cpu = conf_common_is_pin_cpu(conf);
    if (is_pin_cpu) {
        pin_thread(num_thread);
    }

    info_t* infos = (info_t*)calloc(num_thread, sizeof(info_t));
    for (int i=0; i<num_thread; i++) {
        char tmp[100];
        sprintf(tmp, "rb%02d", i);
        infos[i].rb = create_ringbuffer_shm(tmp, sizeof(tuple_t));
        infos[i].id = i;
    }

	pthread_attr_t attr;
	int s = pthread_attr_init(&attr);
    if (s != 0) {
		LOG_ERR("pthread_attr_init: %s\n", strerror(errno));
	}
	for (int i=0; i<num_thread; i++) {
        sem_init(&infos[i].sem, 1, 0);
		s = pthread_create(&infos[i].thread, &attr,
                                  &thread_handler, infos+i);
    	if (s != 0)
			LOG_ERR("pthread_create: %s\n", strerror(errno));
        // wait for ready
        sem_wait(&infos[i].sem);
	}

    tuple_t t_ovs;
    memset(&t_ovs, 0, sizeof(struct Tuple));

    unsigned long long interval_len = conf_common_interval_len(conf);
    uint64_t start_ts = now_us();
    uint64_t cur_ts;

    int index = 0;
    unsigned long long last_interval = 0;
    uint64_t interval_cnt = 0;
    uint64_t interval_byte = 0;

    start_ringbuffer_kshm(krb);
    while (1) {
        while (read_ringbuffer(krb, &t_ovs) < 0) {}
        
        unsigned long long cur_interval = (unsigned long long)(interval_cnt/interval_len);
        if (last_interval == 0) {
            last_interval = cur_interval;
        }
        else if (cur_interval > last_interval) {
            cur_ts = now_us();
            tuple_t t;
            memset(&t, 0, sizeof(struct Tuple));
            packet_stat.used_time += cur_ts - start_ts;
            interval_cnt = 0;
            interval_byte = 0;
    
            // notify threads
            t.flag = TUPLE_PUNC;
            for (int i=0; i<num_thread; i++) {
                while (write_ringbuffer(infos[i].rb, &t, sizeof(tuple_t)) < 0) {};
                flush_ringbuffer(infos[i].rb);
                sem_wait(&infos[i].sem);
            }
            t.flag = TUPLE_NORMAL;

            last_interval = cur_interval;
            start_ts = now_us();
        }

        while (write_ringbuffer(infos[index].rb, &t_ovs, sizeof(tuple_t)) < 0) {};
        interval_cnt++;
        interval_byte += t_ovs.size;
        index = (index + 1) % num_thread;
    }

    stop_ringbuffer_kshm(krb);

    // notify threads
    t_ovs.flag = TUPLE_TERM;
    for (int i=0; i<num_thread; i++) {
        while (write_ringbuffer(infos[i].rb, &t_ovs, sizeof(tuple_t)) < 0) {};
        flush_ringbuffer(infos[i].rb);
    }

    cur_ts = now_us();
    packet_stat.used_time += cur_ts - start_ts;
    report_final_stat();

    for (int i=0; i<num_thread; i++) {
        unsigned long long cnt;
        pthread_join(infos[i].thread, (void**)&cnt);
        LOG_MSG("Thread %d process %llu packets\n", i, cnt);
    }

    for (int i=0; i<num_thread; i++) {
        close_ringbuffer_shm(infos[i].rb);
    }
    free(infos);

    return 0;
}
