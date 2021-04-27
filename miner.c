#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <mqueue.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <semaphore.h>
#include "miner.h"

#define PRIME 99997669
#define BIG_X 435679812
#define BIG_Y 100001819


/*******************
 ** Main function **
 *******************/

int main(int argc, char **argv) {
    NetData *net_data;
    int miner_index; /* To free miners_pid position on shared memory later */

    if (net_register(&net_data, &miner_index) == -1) exit(EXIT_FAILURE);

    /* ...  */

    if (clean(net_data, miner_index) == -1) exit(EXIT_FAILURE);

    exit(EXIT_SUCCESS);
}




/************************
 ** Net Data Functions **
 ************************/

/* Private */
int init_net(NetData *netStruct, int *miner_ind) {

    /* Initialize a mutex to access net data structure */
    if (sem_init(&(netStruct->sem_net_mutex), 1, 0) == -1) {
        perror("Error: could not initialize a semaphore\nsem_init");
        shm_unlink(SHM_NAME_NET);
        return EXIT_FAILURE;
    }

    netStruct->miners_pid[0] = getpid();
    /* Initialize all array elements to -1, to indicate there is no active miner */
    for (int i=1; i<MAX_MINERS; i++) netStruct->miners_pid[i] = -1;
    netStruct->total_miners = 1;
    /* Initialize all array elements to -1, to indicate it is not a valid vote */
    for (int i=0; i<MAX_MINERS; i++) netStruct->voting_pool[i] = -1;

    if (sem_post(&(netStruct->sem_net_mutex))==-1) return EXIT_FAILURE;

    *miner_ind = 0;

    return EXIT_SUCCESS;
}

/* Private */
int join_net(NetData *netStruct, int *miner_ind) {
    int i;

    if (sem_wait(&(netStruct->sem_net_mutex))==-1) return EXIT_FAILURE;

    for (i=0; netStruct->miners_pid[i]!=-1 && i<MAX_MINERS; i++);
    if (i >= MAX_MINERS) {
        fprintf(stderr, "Error: max miners amount reached\n");
        return EXIT_FAILURE;
    }
    netStruct->miners_pid[i] = getpid();
    netStruct->total_miners++;
    *miner_ind = i;

    if (sem_post(&(netStruct->sem_net_mutex))==-1) return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

int net_register(NetData **netStruct, int *miner_ind) {
    int shm_net_fd;
    int exist = 0;

    /* Try creating net info structure on share memory */
    if ((shm_net_fd = shm_open(SHM_NAME_NET, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR)) == -1) {
        if (errno == EEXIST) {
            exist = 1;
            /* If structure already exists, open it */
            if ((shm_net_fd = shm_open(SHM_NAME_NET, O_RDWR, 0)) == -1) {
                perror("Error: could not open existing miner net info structure on shared memory\nshm_open");
                return EXIT_FAILURE;
            }
        }
        else {
            perror("Error: could not create miner net info structure on shared memory\nshm_open");
            return EXIT_FAILURE;
        }
    }

    /* If shared memory was created, truncate it */
    if (exist==0 && ftruncate(shm_net_fd, sizeof(NetData))==-1) {
        perror("Error: could not truncate shared memory size to minet net data structure.\nftruncate");
        close(shm_net_fd);
        return EXIT_FAILURE;
    }

    /* Map the memory segment. */
    *netStruct = mmap(NULL, sizeof(NetData), PROT_READ | PROT_WRITE, MAP_SHARED, shm_net_fd, 0);
    close(shm_net_fd);
    if (*netStruct == MAP_FAILED) {
        perror("Error: could not map miner net info structure\nmmap");
        return EXIT_FAILURE;
    }

    if (exist == 0) return init_net(*netStruct, miner_ind);
    else return join_net(*netStruct, miner_ind);
}

int clean(NetData *netStruct, int miner_ind) {

    if (sem_wait(&(netStruct->sem_net_mutex))==-1) return EXIT_FAILURE;

    /* Tell other miners this miner is finished */
    netStruct->miners_pid[miner_ind] = -1;
    netStruct->total_miners--;

    if (netStruct->total_miners <= 0) {
        /* Free shared memory if this is the last miner */
        sem_destroy(&netStruct->sem_net_mutex);
        shm_unlink(SHM_NAME_NET);
    } else {
        if (sem_post(&(netStruct->sem_net_mutex))==-1) return EXIT_FAILURE;
    }

    munmap(netStruct, sizeof(NetData));

    return EXIT_SUCCESS;
}




/**********************
 ** Mining functions **
 **********************/

long int simple_hash(long int number) {
    long int result = (number * BIG_X + BIG_Y) % PRIME;
    return result;
}
