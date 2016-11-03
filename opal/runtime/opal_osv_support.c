/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */

#include "opal/include/opal_config.h" // to define _GNU_SOURCE; must be first include
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "opal/runtime/opal_osv_support.h"
#include <stdio.h>
#include <pthread.h>
#include <sched.h>

#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>
#include <ctype.h>
#include "opal/util/argv.h"
#include <assert.h>
#include <string.h>

/* 
 * Return 1 if inside OSv, 0 otherwise.
 * */
int opal_is_osv() {
    /* osv_execve is imported as weak symbol, it is NULL if not present */
    if(osv_execve) {
        return true;
    }
    else {
        return false;
    }
}

/**
 * Replacement for getpid().
 * In Linux, return usual process ID.
 * In OSv, return thread ID instead of process ID.
 */
pid_t opal_getpid()
{
    pid_t id;
    if(opal_is_osv()) {
        id = syscall(__NR_gettid);
    }
    else {
        id = getpid();
    }
    return id;
}


/**
 * Pin Open MPI thread to vCPU.
 * vCPU number we pin to is based on MPI rank - very simplistic.
 *
 * Note that osv_execve cannot access environ from the caller - getenv("OMPI_COMM_WORLD_RANK")
 * return NULL. So we need to access environ from "userspace".
 */
void hack_osv_thread_pin() {
    if(!opal_is_osv()) {
        //fprintf(stderr, "TTRT hack_osv_thread_pin not on OSv (fyi rank=%d, my_cpu=%d)\n", rank, my_cpu);
        return;
    }

    char* rank_str; /* OMPI_COMM_WORLD_RANK OMPI_COMM_WORLD_LOCAL_RANK OMPI_COMM_WORLD_NODE_RANK */
    // rank_str = getenv("OMPI_COMM_WORLD_LOCAL_RANK");
    rank_str = getenv("OMPI_COMM_WORLD_RANK");
    if(rank_str == NULL) {
        // This is not OMPI worker thread, so maybe it is mpirun program or orted program.
        //fprintf(stderr, "TTRT hack_osv_thread_pin no OMPI_COMM_WORLD_LOCAL_RANK in env\n");
        return;
    }
    int rank = atoi(rank_str);
    char* cpu_count_str = getenv("OSV_CPUS");
    int cpu_count, my_cpu;
    if(cpu_count_str == NULL) {
        //fprintf(stderr, "TTRT hack_osv_thread_pin no OSV_CPUS in env\n");
        return;
    }

    cpu_count = atoi(cpu_count_str);
    my_cpu = (rank+0) % cpu_count;

    int err;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(my_cpu, &cpuset);
    err = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    //fprintf(stderr, "TTRT hack_osv_thread_pin pthread_setaffinity_np to cpu %d, rank=%d, ret = %d\n", my_cpu, rank, err);
}

static int tcp_connect(char *host, int port) {
  int sockfd, n;
  struct sockaddr_in serv_addr;
  struct hostent *server;

  fprintf(stderr, "HTTP Connecting to %s:%d\n", host, port);
  /* Create a socket point */
  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("ERROR opening socket");
    return -1;
  }
  /* resolve DNS name */
  server = gethostbyname(host);
  if (server == NULL) {
    fprintf(stderr,"ERROR, no such host %s\n", host);
    return -1;
  }
  /* connect to server */
  bzero((char *) &serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  bcopy((char *)server->h_addr, (char *)&serv_addr.sin_addr.s_addr, server->h_length);
  serv_addr.sin_port = htons(port);
  /* Now connect to the server */
  if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
    perror("ERROR connecting");
    return -1;
  }

  return sockfd;
}

static http_client_t http_connect(char *host, int port) {
  http_client_t httpc = {NULL, 0, -1};
  httpc.sockfd = tcp_connect(host, port);
  if (httpc.sockfd < 0) {
      return httpc;
  }
  httpc.host = strdup(host); // free
  httpc.port = port;
  return httpc;

}

static int tcp_close(int sockfd) {
    close(sockfd);
}

static int http_close(http_client_t *httpc) {
    if (httpc->sockfd != -1) {
        close(httpc->sockfd);
        httpc->sockfd = -1;
    }
    if (httpc->host) {
        free(httpc->host);
        httpc->host = NULL;
    }
}

static int tcp_write(int sockfd, char* buf, size_t sz) {
    size_t sz2;
    sz2 = write(sockfd, buf, sz);
    if(sz2 < 0) {
        perror("ERROR tcp_write failed");
        exit(1);
    }
    if(sz2 != sz) {
        perror("ERROR tcp_write incomplete");
        exit(1);
    }
    return sz2;
}

static char rfc3986[256] = {0};
static char html5[256] = {0};
static int url_table_setup_done = 0;

void url_encoder_rfc_tables_init() {
    if(url_table_setup_done)
        return;
    url_table_setup_done = 1;

    int i;
    for (i = 0; i < 256; i++) {
        rfc3986[i] = isalnum( i) || i == '~' || i == '-' || i == '.' || i == '_' ? i : 0;
        html5[i] = isalnum( i) || i == '*' || i == '-' || i == '.' || i == '_' ? i : (i == ' ') ? '+' : 0;
    }
}

char *url_encode(char *table, unsigned char *s) {
    url_encoder_rfc_tables_init();
    char *enc0, *enc;
    enc0 = malloc(strlen(s)*3);
    if (enc0 == NULL)
        return NULL;

    enc = enc0;
    for ( ; *s; s++) {
        if (table[*s]) {
            sprintf( enc, "%c", table[*s]);
        }
        else {
            sprintf( enc, "%%%02X", *s);
        }
        while (*++enc);
    }
    return enc0;
}

static int http_send(http_client_t httpc, char* method, char* buf) {
    char buf2[1024*10];
    int pos = 0;

    /* POST/PUT data */
    /*
    HTTP/1.1 could be used to reuse existing tcp connection.
    But OSv REST server is 1.0 only anyway.
    */
    pos += snprintf(buf2+pos, sizeof(buf2)-pos, "%s %s HTTP/1.0\r\n", method, buf);
    if (pos >= sizeof(buf2)) {
        return -1;
    }
    // headers, skip User-Agent
    pos += snprintf(buf2+pos, sizeof(buf2)-pos, "Host: %s:%d\r\n", httpc.host, httpc.port);
    if (pos >= sizeof(buf2)) {
        return -1;
    }
    pos += snprintf(buf2+pos, sizeof(buf2)-pos, "Accept: */*\r\n");
    if (pos >= sizeof(buf2)) {
        return -1;
    }
    // done
    pos += snprintf(buf2+pos, sizeof(buf2)-pos, "\r\n");
    if (pos >= sizeof(buf2)) {
        return -1;
    }

    fprintf(stderr, "HTTP put %s\n", buf2);
    return tcp_write(httpc.sockfd, buf2, strlen(buf2));
}

static int http_put(http_client_t httpc, char* buf) {
    return http_send(httpc, "PUT", buf);
}

static int http_post(http_client_t httpc, char* buf) {
    return http_send(httpc, "POST", buf);
}

static int tcp_read(int sockfd, char* buf, size_t sz) {
    size_t sz2;
    bzero(buf, sz);
    sz2 = read(sockfd, buf, sz);
    if(sz2 < 0) {
        perror("ERROR tcp_read failed");
        exit(1);
    }
    return sz2;
}

static int http_read(http_client_t httpc, char* buf, size_t sz) {
    int ret;
    ret = tcp_read(httpc.sockfd, buf, sz);
    fprintf(stderr, "HTTP received %s\n", buf);
    // TODO check buf
    return 200;
}

/*
Start program on remote OSv VM.
Return 0 on success, negative number on error.

It does:
POST /env/PATH?val=%2Fusr%2Fbin%3A%2Fusr%2Flib
PUT /app/?command=...
*/
int opal_osvrest_run(char *host, int port, char **argv) {
    int ret = -1;
    /*
    Setup env PATH. It should not be empty, or even /usr/lib/orted.so (with abs path)
    will not be "found" on PATH;
    */
    char env_path[] = "/env/PATH?val=%2Fusr%2Fbin%3A%2Fusr%2Flib";

    /* Assemble command to run on OSv VM. */
    char* var;
    var = opal_argv_join(argv, ' ');
    /* OPAL_OUTPUT_VERBOSE((1, orte_plm_base_framework.framework_output,
                         "%s plm:osv_rest: host %s:%d, cmd %s)",
                         ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), host, port, var)); */
    fprintf(stderr, "TTRT osv_rest_run host %s:%d, cmd %s", host, port, var);
    /* urlencode data part */
    char *var_enc = url_encode(html5, var);
    fprintf(stderr, "HTTP run var     %s\n", var);
    fprintf(stderr, "HTTP run var_enc %s\n", var_enc);
    /* build full URL */
    size_t var2_maxlen = strlen(var) + 100;
    char *var2 = malloc(var2_maxlen);
    if (var2==NULL) {
        goto DONE;
    }
    snprintf(var2, var2_maxlen, "/app/?command=%s", var_enc);

    http_client_t httpc;
    char buf[1024];
    httpc = http_connect(host, port);
    if (httpc.sockfd < 0) {
        goto DONE;
    }
    http_post(httpc, env_path);
    http_read(httpc, buf, sizeof(buf));
    http_close(&httpc);

    httpc = http_connect(host, port);
    if (httpc.sockfd < 0) {
        goto DONE;
    }
    http_put(httpc, var2);
    http_read(httpc, buf, sizeof(buf));
    ret = 0;

DONE:
    http_close(&httpc);
    if(var) {
        free(var);
    }
    if (var_enc) {
        free(var_enc);
    }
    if(var2) {
        free(var2);
    }
    return ret;
}
