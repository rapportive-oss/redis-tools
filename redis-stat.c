/* Redis stat utility.
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * This software is NOT released under a free software license.
 * It is a commercial tool, under the terms of the license you can find in
 * the COPYING file in the Redis-Tools distribution.
 */

#include "fmacros.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <limits.h>
#include <math.h>
#include <sys/time.h>

#include "sds.h"
#include "zmalloc.h"
#include "hiredis.h"
#include "anet.h"
#include "utils.h"

#define REDIS_NOTUSED(V) ((void) V)

#define STAT_VMSTAT 0
#define STAT_VMPAGE 1
#define STAT_OVERVIEW 2
#define STAT_ONDISK_SIZE 3
#define STAT_LATENCY 4

static struct config {
    char *hostip;
    int hostport;
    int delay;
    int stat; /* The kind of output to produce: STAT_* */
    int samplesize;
    int logscale;
    char *auth;
} config;

static long long microseconds(void) {
    struct timeval tv;
    long long mst;

    gettimeofday(&tv, NULL);
    mst = ((long long)tv.tv_sec)*1000000;
    mst += tv.tv_usec;
    return mst;
}

void usage(char *wrong) {
    if (wrong)
        printf("Wrong option '%s' or option argument missing\n\n",wrong);
    printf(
"Usage: redis-stat <type> ... options ...\n\n"
"Statistic types:\n"
" overview (default)   Print general information about a Redis instance.\n"
" vmstat               Print information about Redis VM activity.\n"
" vmpage               Try to guess the best vm-page-size for your dataset.\n"
" ondisk-size          Stats and graphs about values len once stored on disk.\n"
" latency              Measure Redis server latency.\n"
"\n"
"Options:\n"
" host <hostname>      Server hostname (default 127.0.0.1)\n"
" port <hostname>      Server port (default 6379)\n"
" auth <password>      Server password (default none)\n"
" delay <milliseconds> Delay between requests (default: 1000 ms, 1 second).\n"
" samplesize <keys>    Number of keys to sample for 'vmpage' and 'ondisk-size'.\n"
" logscale             User power-of-two logarithmic scale in graphs.\n"
);
    exit(1);
}

static int parseOptions(int argc, char **argv) {
    int i;

    for (i = 1; i < argc; i++) {
        int lastarg = i==argc-1;
        
        if (!strcmp(argv[i],"host") && !lastarg) {
            char *ip = zmalloc(32);
            if (anetResolve(NULL,argv[i+1],ip) == ANET_ERR) {
                printf("Can't resolve %s\n", argv[i]);
                exit(1);
            }
            config.hostip = ip;
            i++;
        } else if (!strcmp(argv[i],"port") && !lastarg) {
            config.hostport = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"auth") && !lastarg) {
            config.auth = strdup(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"delay") && !lastarg) {
            config.delay = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"samplesize") && !lastarg) {
            config.samplesize = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"vmstat")) {
            config.stat = STAT_VMSTAT;
        } else if (!strcmp(argv[i],"vmpage")) {
            config.stat = STAT_VMPAGE;
        } else if (!strcmp(argv[i],"overview")) {
            config.stat = STAT_OVERVIEW;
        } else if (!strcmp(argv[i],"ondisk-size")) {
            config.stat = STAT_ONDISK_SIZE;
        } else if (!strcmp(argv[i],"latency")) {
            config.stat = STAT_LATENCY;
        } else if (!strcmp(argv[i],"logscale")) {
            config.logscale = 1;
        } else if (!strcmp(argv[i],"help")) {
            usage(NULL);
        } else {
            usage(argv[i]);
        }
    }
    return i;
}

/* Return the specified INFO field from the INFO command output "info".
 * The result must be released calling sdsfree().
 *
 * If the field is not found NULL is returned. */
static sds getInfoField(char *info, char *field) {
    char *p = strstr(info,field);
    char *n1, *n2;

    if (!p) return NULL;
    p += strlen(field)+1;
    n1 = strchr(p,'\r');
    n2 = strchr(p,',');
    if (n2 && n2 < n1) n1 = n2;
    return sdsnewlen(p,(n1-p));
}

/* Like the above function but automatically convert the result into
 * a long. On error (missing field) LONG_MIN is returned. */
static long getLongInfoField(char *info, char *field) {
    sds val = getInfoField(info,field);
    long l;

    if (!val) return LONG_MIN;
    l = strtol(val,NULL,10);
    sdsfree(val);
    return l;
}

static void overview(int fd) {
    redisReply *r;
    int c = 0;
    long aux, requests = 0;

    while(1) {
        char buf[64];
        int j;

        r = redisCommand(fd,"INFO");
        if (r->type == REDIS_REPLY_ERROR) {
            printf("ERROR: %s\n", r->reply);
            exit(1);
        }

        if ((c % 20) == 0) {
            printf(
" ------- data ------ ------------ load ----------------------------- - childs -\n");
            printf(
" keys      used-mem  clients blpops  requests            connections\n");
        }

        /* Keys */
        aux = 0;
        for (j = 0; j < 20; j++) {
            long k;

            sprintf(buf,"db%d:keys",j);
            k = getLongInfoField(r->reply,buf);
            if (k == LONG_MIN) continue;
            aux += k;
        }
        sprintf(buf,"%ld",aux);
        printf(" %-10s",buf);

        /* Used memory */
        aux = getLongInfoField(r->reply,"used_memory");
        bytesToHuman(buf,aux);
        printf("%-9s",buf);

        /* Clients */
        aux = getLongInfoField(r->reply,"connected_clients");
        sprintf(buf,"%ld",aux);
        printf(" %-8s",buf);

        /* Blocked (BLPOPPING) Clients */
        aux = getLongInfoField(r->reply,"blocked_clients");
        sprintf(buf,"%ld",aux);
        printf("%-8s",buf);

        /* Requets */
        aux = getLongInfoField(r->reply,"total_commands_processed");
        sprintf(buf,"%ld (+%ld)",aux,aux-requests);
        printf("%-19s",buf);
        requests = aux;

        /* Connections */
        aux = getLongInfoField(r->reply,"total_connections_received");
        sprintf(buf,"%ld",aux);
        printf(" %-12s",buf);

        /* Childs */
        aux = getLongInfoField(r->reply,"bgsave_in_progress");
        aux |= getLongInfoField(r->reply,"bgrewriteaof_in_progress") << 1;
        switch(aux) {
        case 0: break;
        case 1:
            printf("BGSAVE");
            break;
        case 2:
            printf("AOFREWRITE");
            break;
        case 3:
            printf("BGSAVE+AOF");
            break;
        }

        printf("\n");
        freeReplyObject(r);
        usleep(config.delay*1000);
        c++;
    }
}

static void vmstat(int fd) {
    redisReply *r;
    int c = 0;
    long aux, pagein = 0, pageout = 0, usedpages = 0, usedmemory = 0;
    long swapped = 0;

    while(1) {
        char buf[64];

        r = redisCommand(fd,"INFO");
        if (r->type == REDIS_REPLY_ERROR) {
            printf("ERROR: %s\n", r->reply);
            exit(1);
        }

        if ((c % 20) == 0) {
            printf(
" --------------- objects --------------- ------ pages ------ ----- memory -----\n");
            printf(
" load-in  swap-out  swapped   delta      used     delta      used     delta    \n");
        }

        /* pagein */
        aux = getLongInfoField(r->reply,"vm_stats_swappin_count");
        if (aux == LONG_MIN) {
            printf("\nError: Redis instance has VM disabled?\n");
            exit(1);
        }
        sprintf(buf,"%ld",aux-pagein);
        pagein = aux;
        printf(" %-9s",buf);

        /* pageout */
        aux = getLongInfoField(r->reply,"vm_stats_swappout_count");
        sprintf(buf,"%ld",aux-pageout);
        pageout = aux;
        printf("%-9s",buf);

        /* Swapped objects */
        aux = getLongInfoField(r->reply,"vm_stats_swapped_objects");
        sprintf(buf,"%ld",aux);
        printf(" %-10s",buf);

        sprintf(buf,"%ld",aux-swapped);
        if (aux-swapped == 0) printf(" ");
        else if (aux-swapped > 0) printf("+");
        swapped = aux;
        printf("%-10s",buf);

        /* used pages */
        aux = getLongInfoField(r->reply,"vm_stats_used_pages");
        sprintf(buf,"%ld",aux);
        printf("%-9s",buf);

        sprintf(buf,"%ld",aux-usedpages);
        if (aux-usedpages == 0) printf(" ");
        else if (aux-usedpages > 0) printf("+");
        usedpages = aux;
        printf("%-9s",buf);

        /* Used memory */
        aux = getLongInfoField(r->reply,"used_memory");
        bytesToHuman(buf,aux);
        printf(" %-9s",buf);

        bytesToHuman(buf,aux-usedmemory);
        if (aux-usedmemory == 0) printf(" ");
        else if (aux-usedmemory > 0) printf("+");
        usedmemory = aux;
        printf("%-9s",buf);

        printf("\n");
        freeReplyObject(r);
        usleep(config.delay*1000);
        c++;
    }
}

size_t getSerializedLen(int fd, char *key) {
    redisReply *r;
    size_t sl = 0;

    /* The value may be swapped out, try to load it */
    r = redisCommand(fd,"GET %s",key);
    freeReplyObject(r);

    r = redisCommand(fd,"DEBUG OBJECT %s",key);
    if (r->type == REDIS_REPLY_STRING) {
        char *p;

        p = strstr(r->reply,"serializedlength:");
        if (p) sl = strtol(p+17,NULL,10);
    } else {
        printf("%s\n", r->reply);
    }
    freeReplyObject(r);
    return sl;
}

#define SAMPLE_SERIALIZEDLEN 0
#define SAMPLE_TYPE 1
#define SAMPLE_REFCOUNT 2
static size_t *sampleDataset(int fd, int type) {
    redisReply *r;
    size_t *samples = zmalloc(config.samplesize*sizeof(size_t));
    size_t totsl = 0, deltasum, avg;
    int j;

    printf("Sampling %d random keys from DB 0...\n", config.samplesize);
    for (j = 0; j < config.samplesize; j++) {
        size_t sl = 0;

        /* Select a RANDOM key */
        r = redisCommand(fd,"RANDOMKEY");
        if (r->type == REDIS_REPLY_NIL) {
            printf("Sorry but DB 0 is empty\n");
            exit(1);
        } else if (r->type == REDIS_REPLY_ERROR) {
            printf("Error: %s\n", r->reply);
            exit(1);
        }
        /* Store the lenght of this object in our sampling vector */
        if (type == SAMPLE_SERIALIZEDLEN) {
            sl = getSerializedLen(fd, r->reply);
            freeReplyObject(r);

            if (sl == 0) {
                /* Problem getting the  length of this object, don't count
                 * this try. */
                j--;
                continue;
            }
        }

        samples[j] = sl;
        totsl += sl;
    }
    printf("  Average: %zu\n", totsl/config.samplesize);

    deltasum = 0, avg = totsl/config.samplesize;
    for (j = 0; j < config.samplesize; j++) {
        long delta = avg-samples[j];

        deltasum += delta*delta;
    }
    printf("  Standard deviation: %.2lf\n\n",
        sqrt(deltasum/config.samplesize));
    return samples;
}

/* The following function implements the "vmpage" statistic, that is,
 * it tries to perform a few simulations with data gained from the dataset
 * in order to understand what's the best vm-page-size for your dataset.
 *
 * How dows it work? We use VMPAGE_PAGES pages of different sizes, and simulate
 * adding data with sizes sampled from the real dataset, in a random way.
 * While doing this we take stats about how efficiently our application is
 * using the swap file with every given page size. The best will win.
 *
 * Why the thing we take "fixed" is the number of pages? It's our constant
 * as Redis will use one bit of RAM for every page in the swap file, so we
 * want to optimized page size while the number of pages is taken fixed. */
#define VMPAGE_PAGES 1000000
static void vmpage(int fd) {
    int j, pagesize, bestpagesize = 0;
    double bestscore = 0;
    size_t *samples;

    samples = sampleDataset(fd,SAMPLE_SERIALIZEDLEN);
    printf("Simulate fragmentation with different page sizes...\n");
    for (pagesize = 8; pagesize <= 1024*64; pagesize*=2) {
        size_t totpages = VMPAGE_PAGES;
        unsigned char *pages = zmalloc(totpages);
        size_t stored_bytes, used_pages;
        double score;

        printf("%d: ",pagesize);
        fflush(stdout);
        stored_bytes = used_pages = 0;
        memset(pages,0,totpages);
        while(1) {
            int r = random() % config.samplesize;
            size_t bytes_needed = samples[r];
            size_t pages_needed = (bytes_needed+(pagesize-1))/pagesize;

            for(j = 0; j < 200; j++) {
                size_t off = random()%(totpages-(pages_needed-1));
                size_t i;

                for (i = off; i < off+pages_needed; i++)
                    if (pages[i] != 0) break;
                if (i == off+pages_needed) {
                    memset(pages+off,1,pages_needed);
                    used_pages += pages_needed;
                    stored_bytes += bytes_needed;
                    break;
                }
            }
            if (j == 200) break;
        }
        printf("bytes per page: %.2lf, space efficiency: %.2lf%%\n",
            (double)stored_bytes/totpages,
            ((double)stored_bytes*100)/(totpages*pagesize));
        score = ((double)stored_bytes/totpages)*
                (((double)stored_bytes*100)/(totpages*pagesize));
        if (bestpagesize == 0 || bestscore < score) {
            bestpagesize = pagesize;
            bestscore = score;
        }
        zfree(pages);
    }
    printf("\nThe best compromise between bytes per page and swap file size: %d\n", bestpagesize);
    zfree(samples);
}

#define SCALE_POWEROFTWO 0
#define SCALE_LINEAR_SMALL 1
#define SCALE_LINEAR_MED 2
#define SCALE_LINEAR_LARGE 3
#define SCALE_LINEAR_AUTO 4
#define GRAPH_ROWS 20
#define GRAPH_BAR_LEN 50
static void samplesToGraph(size_t *samples, int scaletype) {
    size_t freq[GRAPH_ROWS];
    size_t scale[GRAPH_ROWS];
    size_t max = 0, sum = 0;
    int j, i, high;

    /* Best linear scale auto detection */
    if (scaletype == SCALE_LINEAR_AUTO) {
        scaletype = SCALE_LINEAR_SMALL;
        for (j = 0; j < config.samplesize; j++) {
            if (scaletype == SCALE_LINEAR_SMALL && samples[j] > 1*GRAPH_ROWS)
                scaletype = SCALE_LINEAR_MED;
            if (scaletype == SCALE_LINEAR_MED && samples[j] > 5*GRAPH_ROWS) {
                scaletype = SCALE_LINEAR_LARGE;
                break; /* there is no scale bigger than this. */
            }
        }
    }

    /* Initialize the frequency table and the scale. */
    memset(freq,0,sizeof(freq));
    for (j = 0; j < GRAPH_ROWS; j++) {
        switch(scaletype) {
        case SCALE_POWEROFTWO:
            scale[j] = (size_t) pow(2,j);
            break;
        case SCALE_LINEAR_SMALL:
            scale[j] = j+1;
            break;
        case SCALE_LINEAR_MED:
            scale[j] = (j+1)*5;
            break;
        case SCALE_LINEAR_LARGE:
            scale[j] = (j+1)*50;
            break;
        }
    }
    /* Increment the frequency table accordingly to the sample value */
    for (j = 0; j < config.samplesize; j++) {
        i = GRAPH_ROWS-1;
        while (i > 0 && scale[i-1] >= samples[j]) i--;
        freq[i]++;
        printf("SAMPLE: %ld\n", samples[j]);
    }
    /* Check what's the highest non-zero frequency element, so we can avoid
     * printing the part of the histogram that's empty. We also need the
     * max value in the freq table. */
    for (high = GRAPH_ROWS-1; high > 0; high--)
        if (freq[high]) break;
    for (j = 0; j <= high; j++) {
        if (max < freq[j]) max = freq[j];
        sum += freq[j];
    }
    /* Our ASCII art business can start */
    for (j = 0; j <= high; j++) {
        char buf[64];
        char bar[GRAPH_BAR_LEN+1];
        int barlen;

        if (j != high)
            sprintf(buf,"<= %ld",(long)scale[j]);
        else
            sprintf(buf,">  %ld",(long)scale[j-1]);
        barlen = (freq[j]*GRAPH_BAR_LEN)/max;
        memset(bar,'-',barlen);
        bar[barlen] = '\0';
        printf("%-13s |%s (%.2f%%)\n", buf, bar,
            (float)freq[j]*100/sum);
    }
    zfree(samples);
}

static void ondiskSize(int fd) {
    size_t *samples;

    samples = sampleDataset(fd,SAMPLE_SERIALIZEDLEN);
    if (config.logscale) {
        samplesToGraph(samples, SCALE_POWEROFTWO);
    } else {
        samplesToGraph(samples, SCALE_LINEAR_AUTO);
    }
}

static void latency(int fd) {
    redisReply *r;
    long long start;
    int seq = 1;

    while(1) {
        start = microseconds();
        r = redisCommand(fd,"PING");
        freeReplyObject(r);
        printf("%d: %.2f ms\n",seq,(double)(microseconds()-start)/1000);
        usleep(config.delay*1000);
        seq++;
    }
}

int main(int argc, char **argv) {
    redisReply *r;
    int fd;

    config.hostip = "127.0.0.1";
    config.hostport = 6379;
    config.stat = STAT_OVERVIEW;
    config.delay = 1000;
    config.samplesize = 10000;
    config.logscale = 0;
    config.auth = NULL;

    parseOptions(argc,argv);

    r = redisConnect(&fd,config.hostip,config.hostport);
    if (r != NULL) {
        printf("Error connecting to Redis server: %s\n", r->reply);
        freeReplyObject(r);
        exit(1);
    }

    if (config.auth != NULL) {
      r = redisCommand(fd, "AUTH %s", config.auth);
      if (r == NULL) {
        printf("No reply to AUTH command, aborting.\n");
        exit(1);
      } else if (r->type == REDIS_REPLY_ERROR) {
        printf("AUTH failed: %s\n", r->reply);
        freeReplyObject(r);
        exit(1);
      }
      printf("AUTH succeeded.\n");
    }

    switch(config.stat) {
    case STAT_VMSTAT:
        vmstat(fd);
        break;
    case STAT_VMPAGE:
        vmpage(fd);
        break;
    case STAT_OVERVIEW:
        overview(fd);
        break;
    case STAT_ONDISK_SIZE:
        ondiskSize(fd);
        break;
    case STAT_LATENCY:
        latency(fd);
        break;
    }
    return 0;
}
