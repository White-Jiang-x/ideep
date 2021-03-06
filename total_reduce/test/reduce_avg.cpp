// Author: Ma, Guokai (guokai.ma@intel.com)
// Copyright 2018 www.intel.com
// This code is modified to demostrate total reduce, an allreduce implementation for repetitive occuring pattern

// Author: Wes Kendall
// Copyright 2013 www.mpitutorial.com
// This code is provided freely with the tutorials on mpitutorial.com. Feel
// free to modify it for your own use. Any distribution of the code must
// either provide a link to www.mpitutorial.com or keep this header intact.
//
//
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <mpi.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include <ideep.hpp>
#include <ideep_pin_singletons.hpp>

#define ITER 10
#define PAYLOAD_COUNT 1
#define SMALL_SCALE 0.1
#define SMALL_SIZE 3
//#define REVERSE_ISSUE
#define ARTIFICAL_NUMBER
#define RUN_INPLACE 1
#define RUN_NON_INPLACE 0
//#define CALLBACK

struct timeval t_begin;

void init_time(void)
{
    gettimeofday(&t_begin, NULL);
}

float get_time(void)
{
    struct timeval t_now;
    gettimeofday(&t_now, NULL);
    return t_now.tv_sec-t_begin.tv_sec+(t_now.tv_usec-t_begin.tv_usec)/1000000.0;
}

static inline int get_layer_size(int id, int num_elements)
{
    if (id==0) return num_elements;
    return num_elements*SMALL_SCALE+SMALL_SIZE;
}

static inline float gen_rand()
{
    return rand()/(float)RAND_MAX+0.01;
}

// Creates an array of random numbers. Each number has a value from 0 - 1
float *create_rand_sum(int num_elements, int id, int world_size) {
    srand(id);
    float *buf = (float *)malloc(sizeof(float) * num_elements);
    for (int i = 0; i < num_elements; i++) {
        buf[i] = 0.0;
    }
    for (int node=0; node <world_size; node++) {
        for (int i = 0; i < num_elements; i++) {
            buf[i] += gen_rand();
        }
    }
    return buf;
}

float *create_rand_nums(int num_elements, int id, int rank) {
    srand(id);
    float *buf = (float *)malloc(sizeof(float) * num_elements);
    for (int node=0; node<rank; node++) {
        for (int i = 0; i<num_elements; i++) {
            buf[i] = gen_rand();
        }
    }
    for (int i = 0; i<num_elements; i++) {
        buf[i] = gen_rand();
    }
    return buf;
}

static inline float calc_val(int id, int rank, int index)
{
    return (id+1)+(rank+1)/100.0+(index+1)/10000.0;
}

float *create_artifical_sum(int num_elements, int id, int world_size)
{
    float *buf = (float *)malloc(sizeof(float) * num_elements);
    for (int rank=0; rank<world_size; rank++) {
        for (int i = 0; i < num_elements; i++) {
            if (rank == 0) {
                buf[i] = calc_val(id, rank, i);
            } else {
                buf[i] += calc_val(id, rank, i);
            }
        }
    }
    return buf;
}

float *create_artifical_nums(int num_elements, int id, int rank)
{
    float *buf = (float *)malloc(sizeof(float) * num_elements);
    for (int i = 0; i < num_elements; i++) {
        buf[i] = calc_val(id, rank, i);
    }
    return buf;
}

void calc_delta(int id, float* buf1, float* buf2, size_t num_elements)
{
    // Clean up
    float total = 0, max_delta = 0, first_delta = 0;
    int total_diff = 0;
    for (size_t i=0; i<num_elements; i++) {
        float delta = buf1[i] - buf2[i];
        if (delta < 0.0) delta = -delta;
        total = total + delta;
        if (delta > max_delta) {
            max_delta = delta;
        }
        if (delta > 0.01 && !(first_delta > 0.0)) {
            first_delta = delta;
        }
        if (delta/buf1[i] > 0.001) {
            total_diff ++;
            if (total_diff <= 3)
                printf("\nindex=%ld buf1=%f buf2=%f", i, buf1[i], buf2[i]);
        }
    }
    if (total_diff > 0) {
        printf ("\nid=%d, max_delta %f num_elems %ld, total_diff=%d\n", id, max_delta, num_elements, total_diff);
        exit (1);
    }
}

#ifdef CALLBACK
void callback (int id)
{
    printf ("layer %d finished\n", id);
}
#else
#define callback NULL
#endif

int main(int argc, char** argv)
{
    float start, end, span, eff_bw;
    if (argc != 2) {
        fprintf(stderr, "Usage: avg num_elements\n");
        exit(1);
    }

    system("/bin/hostname");
    int num_elements = atoi(argv[1]);

    init_time();

    ideep::distribute::init(6);

    int world_size = ideep::distribute::get_world_size();
    int world_rank = ideep::distribute::get_rank();

    if(world_rank==0) printf ("%d: number of bytes per node is %d, world_size=%d, rank=%d\n\n", getpid(), num_elements*4, world_size, world_rank);

    float *send_buf[PAYLOAD_COUNT];
    float *send_buf_ref[PAYLOAD_COUNT];
    float *recv_buf_ref[PAYLOAD_COUNT];

    for (int layer=0; layer<PAYLOAD_COUNT; layer++) {
        if (world_rank == 0) {
            printf ("initialize layer %d\n", layer);
        }
        send_buf[layer] =
            #ifdef ARTIFICAL_NUMBER
            create_artifical_nums(get_layer_size(layer, num_elements), layer, world_rank);
            #else
            create_rand_nums(get_layer_size(layer, num_elements), layer, world_rank);
            #endif
        send_buf_ref[layer] = (float *)malloc(sizeof(float) * get_layer_size(layer, num_elements));
        memcpy(send_buf_ref[layer], send_buf[layer], sizeof(float) * get_layer_size(layer, num_elements));
        recv_buf_ref[layer] =
            #ifdef ARTIFICAL_NUMBER
            create_artifical_sum(get_layer_size(layer, num_elements), layer, world_size);
            #else
            create_rand_sum(get_layer_size(layer, num_elements), layer, world_size);
            #endif
    }


    float *recv_buf[PAYLOAD_COUNT];
    for (int i=0; i<PAYLOAD_COUNT; i++) {
        recv_buf[i] = (float *)malloc(sizeof(float) * get_layer_size(i, num_elements));
    }

    size_t total_elements;

    #if RUN_INPLACE
    start = get_time();
    for (int index=0; index<ITER; index++) {
        if(world_rank==0) printf ("**************total reduce iallreduce, inplace iTER=%d**************************\r", index);

        total_elements = 0;
        for (int i=0; i<PAYLOAD_COUNT; i++) {
            memcpy(recv_buf[i], send_buf[i], get_layer_size(i, num_elements)*sizeof(float));
        }

        #ifndef REVERSE_ISSUE
        for (int i=0; i<PAYLOAD_COUNT; i++) {
            size_t num = get_layer_size(i, num_elements);
            total_elements += num;
            ideep::tensor::dims dim = {(int)num};
            ideep::tensor::data_type type = ideep::tensor::data_type::f32;
            ideep::tensor buffer ({dim, type}, const_cast<float *>(recv_buf[i]));
            ideep::distribute::iallreduce(i, buffer, callback);
        }
        #else
        if (world_rank % 2) {
            for (int i=0; i<PAYLOAD_COUNT; i++) {
                size_t num = get_layer_size(i, num_elements);
                total_elements += num;
                ideep::tensor::dims dim = {(int)num};
                ideep::tensor::data_type type = ideep::tensor::data_type::f32;
                ideep::tensor buffer ({dim, type}, const_cast<float *>(recv_buf[i]));
                ideep::distribute::iallreduce(i, buffer, callback);
            }
        } else {
            for (int i=PAYLOAD_COUNT-1; i>=0; i--) {
                size_t num = get_layer_size(i, num_elements);
                total_elements += num;
                ideep::tensor::dims dim = {(int)num};
                ideep::tensor::data_type type = ideep::tensor::data_type::f32;
                ideep::tensor buffer ({dim, type}, const_cast<float *>(recv_buf[i]));
                ideep::distribute::iallreduce(i, 0, buffer, callback);
            }
        }
        #endif

        #ifndef CALLBACK
        for (int i=PAYLOAD_COUNT-1; i>=0; i--) {
            ideep::distribute::wait(i);
        }
        #endif

        ideep::distribute::barrier();

        for (int i=0; i<PAYLOAD_COUNT; i++) {
            calc_delta(i, recv_buf_ref[i], recv_buf[i], get_layer_size(i, num_elements));
        }
    }
    end = get_time();
    span = (end-start)/ITER;
    eff_bw = 2.0*(world_size-1)/world_size*total_elements * 32/span/1000000000;
    printf ("average iter time = %fs, Effective bandwidth = %f\n", span, eff_bw);
    #endif

    #if RUN_NON_INPLACE
    float start = get_time();
    for (int index=0; index<ITER; index++) {
        total_elements = 0;
        if(world_rank==0) printf ("**************total reduce iallreduce, iTER=%d**************************\r", index);

        for (int i=0; i<PAYLOAD_COUNT; i++) {
            for (int j=0; j<get_layer_size(i, num_elements); j++) {
                recv_buf[i][j] = 0.0001*world_rank+0.1*j;
            }
        }

        for (int i=0; i<PAYLOAD_COUNT; i++) {
            size_t num = get_layer_size(i, num_elements);
            total_elements += num;
            ideep::tensor::dims dim = {(int)num};
            ideep::tensor::data_type type = ideep::tensor::data_type::f32;
            ideep::tensor buffer_from ({dim, type}, const_cast<float *>(send_buf[i]));
            ideep::tensor buffer_to ({dim, type}, const_cast<float *>(recv_buf[i]));
            ideep::distribute::iallreduce(i+PAYLOAD_COUNT, i, buffer_from, buffer_to, callback);
        }

        #ifndef CALLBACK
        for (int i=PAYLOAD_COUNT-1; i>=0; i--) {
            ideep::distribute::wait(i+PAYLOAD_COUNT);
        }
        #endif

        ideep::distribute::barrier();

        for (int i=0; i<PAYLOAD_COUNT; i++) {
            calc_delta(i, recv_buf_ref[i], recv_buf[i], get_layer_size(i, num_elements));
        }
    }
    end = get_time();
    span = (end-start)/ITER;
    eff_bw = 2.0*(world_size-1)/world_size*total_elements * 32/span/1000000000;
    printf ("Effective bandwidth = %f\n", eff_bw);
    #endif

    ideep::distribute::finalize();
    exit(0);
}
