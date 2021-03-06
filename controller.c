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
#include "stat.h"

#include "hash.h"
#include "alg_keytbl.h"

extern conf_t* conf;

const double min_thresh = 0.3;

unsigned char* keys = NULL;
int32_t* vals = NULL;
double* confs = NULL;
int max_ret = 1000000;

const char* alg = NULL;

const int n_104 = 104;
int bits_104[104];

void remove_large(SketchLearn_t* skl, rich_tuple_t* list, int n) {
    for (int i=0; i<n; i++) {
        SKL_Update(skl, (unsigned char*)&list[i].key, -list[i].size);
    }
    SKL_CompleteZeroes(skl);
}

int detect_with_thresh(SketchLearn_t* skl, double thresh, 
        rich_tuple_t* list_ret) {

    int width = conf_skl_width(conf);
    int depth = conf_skl_depth(conf);
    int key_len = conf_common_key_len(conf);
    SketchLearn_t* skl_new = SKL_Init(width, depth, key_len);
    SKL_Copy(skl_new, skl);

    int bit = conf_common_key_len(conf);
    double timeout = conf_keytbl_timeout(conf);
    key_tbl_t* tbl_detect = key_tbl_init(bit, 60000, 10, timeout);

    int n_ret = 0;
    rich_tuple_t* list_tmp = (rich_tuple_t*)calloc(max_ret, sizeof(rich_tuple_t));

    // try to extract all flows until no flows satisfy the input thresh
    while (1) {

        // extract
        // LOG_MSG("detect: extract\n");
        unsigned long n = SKL_Identify_Thresh(skl_new, thresh, keys, vals, confs, max_ret); 

        // remove duplicated flows
        // LOG_MSG("detect: remove duplicated flow.\n");
        int n_tmp = 0;
        for (unsigned long i=0; i<n; i++) {
            if (n_tmp == max_ret) {
                LOG_MSG("Reach max ret 1\n");
            }
            if (key_tbl_find(tbl_detect, (tuple_t*)(keys+i*skl->lgn/8)) != NULL) {
                continue;
            }
            key_tbl_entry_p_t record = key_tbl_record(tbl_detect, (tuple_t*)(keys+i*skl->lgn/8), -1);
            record->tuple.size += 1;
            memcpy(list_tmp+n_tmp, keys+i*skl->lgn/8, skl->lgn/8); // flow keys
            list_tmp[n_tmp].size = vals[i]; // roughly flow size
            memcpy(list_tmp[i].conf, confs+i*skl->lgn, skl->lgn*sizeof(double)); // flow confidence
            n_tmp++;
        }
        qsort(list_tmp, n_tmp, sizeof(rich_tuple_t), cmp);

        int new_ret = 0;
        for (int i=0; i<n_tmp; i++) {

            // estimate flow size and drop unreasonable flows
            // LOG_MSG("detect: estimate.\n");
            list_tmp[i].size = 0;
            int32_t est = SKL_Est_Size(skl_new, (unsigned char*)(list_tmp+i), bits_104, n_104);
            if (est <= 0) { 
                continue;
            }

            // drop low-confident flows
            // LOG_MSG("detect: drop low-confident flow.\n");
            qsort(list_tmp[i].conf, skl->lgn, sizeof(double), cmp_lf);
            if (list_tmp[i].conf[skl->lgn*9/10]<0) { 
                continue;
            }
            if (n_ret == max_ret) {
                LOG_MSG("Reach max ret\n");
            }
            memcpy(list_ret+n_ret, list_tmp+i, sizeof(rich_tuple_t));
            list_ret[n_ret].size = est;
            list_tmp[i].size = est;
            
            n_ret++;
            new_ret++;

        }
        remove_large(skl_new, list_tmp, n_tmp);
        if (new_ret == 0) {
            break;
        }
    }
    free(list_tmp);

    key_tbl_destroy(tbl_detect);
    return n_ret;
}

//only do inference.
uint64_t do_inference(SketchLearn_t* skl, tuple_t* list_all_detect, int* n_all_detect) {
    // read true keys as ground truth
    int bit = conf_common_key_len(conf);
    double timeout = conf_keytbl_timeout(conf);
    key_tbl_t* tbl_detect = key_tbl_init(bit, 60000, 10, timeout);

    // data structures for detected keys
    rich_tuple_t* list_detect = (rich_tuple_t*)calloc(max_ret, sizeof(rich_tuple_t));
    int n_detect = 0;
    keys = (unsigned char*)calloc(max_ret,
            sizeof(unsigned char)*skl->lgn/8);
    vals = (int32_t*)calloc(max_ret, sizeof(int32_t));
    confs = (double*)calloc(max_ret*skl->lgn, sizeof(double));
    rich_tuple_t* list_ret = (rich_tuple_t*)calloc(max_ret, sizeof(rich_tuple_t));

    int d = 2;
    uint64_t start_ts = now_us();
    while (1) {
        double thresh = 1.0 / d;
        if (thresh < min_thresh) {
            break;
        }
        
        // try to extarct large flows with a particular threshold
        int n_ret = detect_with_thresh(skl, thresh, list_ret); 
        if (n_ret == 0) {
            break;
        }

        // record extracted flows
        for (int i=0; i<n_ret; i++) {
            if (key_tbl_find(tbl_detect, (tuple_t*)(list_ret+i))== NULL) {
                if (n_detect == max_ret) {
                    LOG_MSG("Reach max ret\n");
                }
                memcpy(list_detect+n_detect, list_ret+i, sizeof(rich_tuple_t));
                n_detect++;

                if (*n_all_detect == max_ret) {
                    LOG_MSG("Reach max ret\n");
                }
                memcpy(list_all_detect+(*n_all_detect), list_ret+i, skl->lgn/8);
                list_all_detect[*n_all_detect].size = list_ret[i].size;

                key_tbl_entry_p_t record = key_tbl_record(tbl_detect, list_all_detect+(*n_all_detect), -1);
                record->tuple.size += list_all_detect[*n_all_detect].size;
                *n_all_detect = *n_all_detect+1;
            }
        }

        // remove extracted flows from sketch
        remove_large(skl, list_ret, n_ret);
        d++;
    }
    uint64_t end_ts = now_us();

    qsort (list_detect, n_detect, sizeof(rich_tuple_t), cmp);
 
    key_tbl_destroy(tbl_detect);
    free(keys);
    free(vals);
    free(confs);
    free(list_ret);
    return end_ts - start_ts;
}

uint64_t do_inference_and_evaluation(int interval, SketchLearn_t* skl, uint64_t tot_size, tuple_t* list_all_detect, int* n_all_detect, FILE* stat_file) {

    char tmp[100];

    // read true keys as ground truth
    int bit = conf_common_key_len(conf);
    double timeout = conf_keytbl_timeout(conf);
    key_tbl_t* tbl_true = key_tbl_init(bit, 60000, 10, timeout);
    key_tbl_t* tbl_detect = key_tbl_init(bit, 60000, 10, timeout);
    sprintf(tmp, "%s/true_flows/true_%d", conf_common_trace_dir(conf), interval);
    key_tbl_read_file(tmp, tbl_true);

    // data structures for detected keys
    rich_tuple_t* list_detect = (rich_tuple_t*)calloc(max_ret, sizeof(rich_tuple_t));
    int n_detect = 0;
    keys = (unsigned char*)calloc(max_ret,
            sizeof(unsigned char)*skl->lgn/8);
    vals = (int32_t*)calloc(max_ret, sizeof(int32_t));
    confs = (double*)calloc(max_ret*skl->lgn, sizeof(double));
    rich_tuple_t* list_ret = (rich_tuple_t*)calloc(max_ret, sizeof(rich_tuple_t));
    sprintf(tmp, "%s/controller/quality_%d", conf_common_trace_dir(conf), interval);
    FILE* output_all = fopen(tmp, "w");

    fprintf(output_all, "tot_size %lu avg %lu\n", tot_size, tot_size/skl->width);

    int d = 2;
    uint64_t start_ts = now_us();
    while (1) {
        double thresh = 1.0 / d;
        if (thresh < min_thresh) {
            break;
        }
        
        // try to extarct large flows with a particular threshold
        int n_ret = detect_with_thresh(skl, thresh, list_ret); 
        if (n_ret == 0) {
            break;
        }

        // record extracted flows
        for (int i=0; i<n_ret; i++) {
            if (key_tbl_find(tbl_detect, (tuple_t*)(list_ret+i))== NULL) {
                if (n_detect == max_ret) {
                    LOG_MSG("Reach max ret\n");
                }
                memcpy(list_detect+n_detect, list_ret+i, sizeof(rich_tuple_t));
                n_detect++;

                if (*n_all_detect == max_ret) {
                    LOG_MSG("Reach max ret\n");
                }
                memcpy(list_all_detect+(*n_all_detect), list_ret+i, skl->lgn/8);
                list_all_detect[*n_all_detect].size = list_ret[i].size;

                key_tbl_entry_p_t record = key_tbl_record(tbl_detect, list_all_detect+(*n_all_detect), -1);
                record->tuple.size += list_all_detect[*n_all_detect].size;
                *n_all_detect = *n_all_detect+1;
            }
        }
        fprintf(output_all, "\tdetect %d keys with thresh %lf\n", n_ret, thresh);

        // remove extracted flows from sketch
        remove_large(skl, list_ret, n_ret);
        d++;
    }
    uint64_t end_ts = now_us();

    // print results
    sprintf(tmp, "%s/controller/res_sketch_%d", conf_common_trace_dir(conf), interval);
    SKL_Print(skl, tmp);
    sprintf(tmp, "%s/controller/large_flows_%d", conf_common_trace_dir(conf), interval);
    FILE* output_keys = fopen(tmp, "w");

    fprintf(output_all, "====Final Results====\n");
    qsort (list_detect, n_detect, sizeof(rich_tuple_t), cmp);
    int n_fp = 0;
    for (int i=0; i<n_detect; i++) {
        print_tuple(output_all, (tuple_t*)(list_detect+i));
        print_tuple(output_keys, (tuple_t*)(list_detect+i));
        for (int j=0; j<skl->lgn; j++) {
            fprintf(output_keys, "%d %lf\n", j, list_detect[i].conf[j]);
        }

        for (uint64_t k=0; k<skl->depth; k++) {
            unsigned long hash = AwareHash((unsigned char*)(list_detect+i), skl->lgn/8,
                skl->hash[k], skl->scale[k], skl->hardner[k]);
            hash=hash % skl->width; 
        }

        key_tbl_entry_t* tp;
        if ((tp=key_tbl_find(tbl_true, (tuple_t*)(list_detect+i))) != NULL) {
            fprintf(output_all, "    true %d (%lf)\n", tp->tuple.size, 1.0*(list_detect[i].size-tp->tuple.size)/tp->tuple.size);
            qsort (list_detect[i].conf, skl->lgn, sizeof(double), cmp_lf);
            fprintf(output_all, "\t%lf %lf %lf %lf %lf %lf\n",
                    list_detect[i].conf[skl->lgn/2],
                    list_detect[i].conf[skl->lgn*3/4],
                    list_detect[i].conf[skl->lgn*4/5],
                    list_detect[i].conf[skl->lgn*9/10],
                    list_detect[i].conf[skl->lgn*99/100],
                    list_detect[i].conf[skl->lgn-1]
                    );
        }
        else {
            fprintf(output_all, "    fp\n");
            n_fp++;
            qsort (list_detect[i].conf, skl->lgn, sizeof(double), cmp_lf);
            fprintf(output_all, "\t%lf %lf %lf %lf %lf %lf\n",
                    list_detect[i].conf[skl->lgn/2],
                    list_detect[i].conf[skl->lgn*3/4],
                    list_detect[i].conf[skl->lgn*4/5],
                    list_detect[i].conf[skl->lgn*9/10],
                    list_detect[i].conf[skl->lgn*99/100],
                    list_detect[i].conf[skl->lgn-1]);
        }
    }
    fclose(output_keys);

    key_tbl_destroy(tbl_true);
    key_tbl_destroy(tbl_detect);
    free(keys);
    free(vals);
    free(confs);
    free(list_ret);
    fclose(output_all);

    return end_ts - start_ts;
}

void evaluate() {
    int n_interval = conf_common_num_interval(conf);

    int width = conf_skl_width(conf);
    int depth = conf_skl_depth(conf);
    int key_len = conf_common_key_len(conf);
    SketchLearn_t* skl = SKL_Init(width, depth, key_len);

    char tmp[100];
    sprintf(tmp, "%s/controller/stat", conf_common_trace_dir(conf));
    FILE* stat_file = fopen(tmp, "w");
    if (stat_file == NULL) {
        LOG_ERR("fail to open file %s\n", tmp); 
    }

    tuple_t* list_all_detect = (tuple_t*)calloc(max_ret, sizeof(tuple_t));
    int n_all_detect = 0;

    for (int i=0; i<n_interval; i++) {
        sprintf(tmp, "%s/sketches/%s_%d", conf_common_trace_dir(conf), alg, i);
        SKL_ReadFile(skl, tmp);

        uint64_t tot_size = 0;
	    for (int i=0; i<skl->size; ++i) {
            int r = i / skl->width;
            if (r > 0) {
                break;
            }
            tot_size += skl->counts[i][0];
	    }

        n_all_detect = 0;
        fprintf(stat_file, "time interval: %d\n", i);
        uint64_t time = do_inference_and_evaluation(i, skl, tot_size, list_all_detect, &n_all_detect, stat_file);
        print_tuple(stat_file, list_all_detect);
        fprintf(stat_file, "decode_time: %lf\n\n", 1.0*time/1000000);
        fprintf(stat_file, "=====================\n\n");
    }
    free(list_all_detect);

    fclose(stat_file);
    SKL_Destroy(skl);

}

void preload() {
    stat_t stat = {0, 0, 0};
    int n_interval = conf_common_num_interval(conf);
    int width = conf_skl_width(conf);
    int depth = conf_skl_depth(conf);
    int key_len = conf_common_key_len(conf);
    SketchLearn_t ** skl_list;
    skl_list = (SketchLearn_t **)calloc(n_interval, sizeof(SketchLearn_t *));
    for(int i=0; i<n_interval; i++) {
        skl_list[i] = SKL_Init(width, depth, key_len);
    }
    
    uint64_t st = now_us();
    for(int i=0; i<n_interval; i++) {
        char tmp[100];
        sprintf(tmp, "%s/sketches/%s_%d", conf_common_trace_dir(conf), alg, i);
        SKL_ReadFile(skl_list[i], tmp);
    }
    stat.time_load = now_us() - st;

    int n_all_detect = 0;
    tuple_t* list_all_detect = (tuple_t*)calloc(max_ret, sizeof(tuple_t));

    st = now_us();
    for(int i=0; i<n_interval; i++) {
        do_inference(skl_list[i], list_all_detect, &n_all_detect);
    }
    stat.time_infer = now_us() - st;

    char tmp[100];
    sprintf(tmp, "%s/stat/preload_%d", conf_common_trace_dir(conf), n_interval);
    FILE *fp = fopen(tmp, "w");
    stat_print(&stat, fp);
    fclose(fp);
}

void load_on_infer() {
    stat_t stat = {0, 0, 0};
    int n_interval = conf_common_num_interval(conf);
    int width = conf_skl_width(conf);
    int depth = conf_skl_depth(conf);
    int key_len = conf_common_key_len(conf);
    SketchLearn_t* skl = SKL_Init(width, depth, key_len);

    tuple_t* list_all_detect = (tuple_t*)calloc(max_ret, sizeof(tuple_t));
    int n_all_detect = 0;

    char tmp[100];
    for (int i=0; i<n_interval; i++) {
        uint64_t st = now_us();
        sprintf(tmp, "%s/sketches/%s_%d", conf_common_trace_dir(conf), alg, i);
        SKL_ReadFile(skl, tmp);
        stat.time_load += now_us() - st;

        st = now_us();
        n_all_detect = 0;
        do_inference(skl, list_all_detect, &n_all_detect);
        stat.time_infer += now_us() - st;
    }
    free(list_all_detect);
    SKL_Destroy(skl);

    sprintf(tmp, "%s/stat/load_on_infer_%d", conf_common_trace_dir(conf), n_interval);
    FILE *fp = fopen(tmp, "w");
    stat_print(&stat, fp);
    fclose(fp);

}

void test_write(int n) {
    int width = conf_skl_width(conf);
    int depth = conf_skl_depth(conf);
    int key_len = conf_common_key_len(conf);
    SketchLearn_t *skl = SKL_Init(width, depth, key_len);
    char tmp[100];
    sprintf(tmp, "%s/sketches/%s_0", conf_common_trace_dir(conf), alg);
    SKL_ReadFile(skl, tmp);

    uint64_t *times;
    times = (uint64_t *)calloc(n + 1, sizeof(uint64_t));
    times[0] = now_us();
    for(int i=0; i<n; i++){
        sprintf(tmp, "%s/write/write_%d", conf_common_trace_dir(conf), i);
        SKL_Print(skl, tmp);
        times[i+1] = now_us();
    }

    sprintf(tmp, "%s/stat/write", conf_common_trace_dir(conf));
    FILE *fp = fopen(tmp, "w");
    fprintf(fp, "Avg: %lu\n", (times[n] - times[0])/n);
    for(int i=0; i<n; i++) {
        fprintf(fp, "%d\t%lu\n", i+1, times[i+1] - times[i]);
    }
    fclose(fp);
}

void test_read(int n) {
    test_write(n);
    int width = conf_skl_width(conf);
    int depth = conf_skl_depth(conf);
    int key_len = conf_common_key_len(conf);
    SketchLearn_t *skl = SKL_Init(width, depth, key_len);
    char tmp[100];
    uint64_t *times;
    times = (uint64_t *)calloc(n + 1, sizeof(uint64_t));
    times[0] = now_us();
    for(int i=0; i<n; i++){
        sprintf(tmp, "%s/write/write_%d", conf_common_trace_dir(conf), i);
        SKL_ReadFile(skl, tmp);
        times[i+1] = now_us();
    }

    sprintf(tmp, "%s/stat/read", conf_common_trace_dir(conf));
    FILE *fp = fopen(tmp, "w");
    fprintf(fp, "Avg: %lu\n", (times[n] - times[0])/n);
    for(int i=0; i<n; i++) {
        fprintf(fp, "%d\t%lu\n", i+1, times[i+1] - times[i]);
    }
    fclose(fp);
}

int main (int argc, char *argv []) {

    // consider all levels during inference
    for (int i=0; i<n_104; i++) {
        bits_104[i] = i;
    }

    if (argc != 2) {
        fprintf(stderr, "Usage: %s [config file]\n", argv[0]);
        exit(-1);
    }
        
    alg = "sketchlearn";
    conf = Config_Init(argv[1]);

    char tmp[100];
    sprintf(tmp, "%s/controller", conf_common_trace_dir(conf));
    mkdir(tmp, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

    sprintf(tmp, "%s/stat", conf_common_trace_dir(conf));
    mkdir(tmp, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

    sprintf(tmp, "%s/write", conf_common_trace_dir(conf));
    mkdir(tmp, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

    while(1) {
        printf("controller >> ");
        fflush(stdout);
        char tmp[100];
        char cmd[100];
        if(fgets(tmp, 100, stdin) == NULL){
            break;
        }
        sscanf(tmp, "%s", cmd);
        //puts(cmd);
        if (strcmp(cmd, "evaluation") == 0) {
            evaluate();
        }
        else if(strcmp(cmd, "test-preload") == 0) {
            LOG_MSG("test preload...\n");
            preload();
        }
        else if(strcmp(cmd, "test-load-on-infer") == 0){
            LOG_MSG("test load-on-infer...\n");
            load_on_infer();
        }
        else if(strcmp(cmd, "test-read") == 0) {
            LOG_MSG("test read sketchs...\n");
            test_read(200);
        }
        else if(strcmp(cmd, "test-write") == 0) {
            LOG_MSG("test write sketchs...\n");
            test_write(200);
        }
        else if(strcmp(cmd, "exit") == 0){
            break;
        }
        else {
            LOG_MSG("unknown command.\n");
        }
        
    }
    return 0;
}
