#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <string.h>
#include <mqueue.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <semaphore.h>
#include "miner.h"

#define PRIME 99997669
#define BIG_X 435679812
#define BIG_Y 100001819
#define TRUE 1
#define FALSE 0


struct worker_args_struct {
    long target;
    long start;
    long end;
    long solution;
};




/**********************
 ** Global variables **
 **********************/
static volatile sig_atomic_t active = TRUE;
static volatile sig_atomic_t flag = TRUE;




/*********************
 ** Signal handlers **
 *********************/
void sigusr2_handler(int sig) {
    flag = FALSE;
}

void sigint_handler(int sig) {
    flag = FALSE;
    active = FALSE;
}




/*******************
 ** Main function **
 *******************/

int main(int argc, char **argv) {
    NetData *net_data;
    Block *plast_block = NULL; /* Last block of the blockchain pointer. Allows access to all the chain */
    Block *shm_block; /* Pointer to shared memeory segment */
    int miner_index; /* To free miners_pid position on shared memory later */
    int win = FALSE;
    int n_workers, n_rounds;
    pthread_t workers[MAX_WORKERS];

    srand(time(NULL));

    if (check_arguments(argc, argv, &n_workers, &n_rounds) != EXIT_SUCCESS) exit(EXIT_FAILURE);

    if (sig_setup() != EXIT_SUCCESS) exit(EXIT_FAILURE);

    if (net_register(&net_data, &shm_block, &miner_index) != EXIT_SUCCESS) exit(EXIT_FAILURE);

    if (miner_main_loop(net_data, shm_block, &plast_block, miner_index, workers, n_workers, n_rounds, &win) != EXIT_SUCCESS) {
        clean(net_data, shm_block, plast_block, miner_index, &win);
        exit(EXIT_FAILURE);
    }

    print_blocks(plast_block);

    if (clean(net_data, shm_block, plast_block, miner_index, &win) != EXIT_SUCCESS) exit(EXIT_FAILURE);

    exit(EXIT_SUCCESS);
}




/*********************
 ** Check arguments **
 *********************/
int check_arguments(int argc, char **argv, int *n_wks, int *n_rds) {

    if (argc < 3) {
        fprintf(stderr, "Error: invalid arguments\n");
        fprintf(stdout, "Usage: %s <number_of_workers> <number_of_rounds>\n", argv[0]);
        return EXIT_FAILURE;
    }

    *n_wks = atoi(argv[1]);
    *n_rds = atoi(argv[2]);

    if (*n_wks > 10 || *n_wks < 1) {
        fprintf(stderr, "Error: number of workers must be a positive number greater than %d\n", MAX_WORKERS);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}




/***************************
 ** Signal handlers setup **
 ***************************/
int sig_setup() {
    struct sigaction act1, act2;

    act1.sa_handler = sigusr2_handler;
    sigemptyset(&(act1.sa_mask));
    act1.sa_flags = 0;

    act2.sa_handler = sigint_handler;
    sigemptyset(&(act2.sa_mask));
    act2.sa_flags = 0;

    if (sigaction(SIGUSR2, &act1, NULL) < 0
        || sigaction(SIGINT, &act2, NULL) < 0) {
        perror("Error: could not set up a signal handler.\nsigaction");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}




/************************
 ** Net Data Functions **
 ************************/
/*
**                     |-----> init_net -----> init_shm_block
**  net_register ------| or
**                     |-----> join_net -----> open_shm_block
 */

/* Private */
int init_shm_block(Block **blockStruct) {
    int shm_block_fd;

    if ((shm_block_fd = shm_open(SHM_NAME_BLOCK, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR)) == -1) {
        if (errno == EEXIST) {
            fprintf(stderr, "Error: block shared memory segment already exists\n");
        }
        else {
            perror("Error: could not create block structure on shared memory\nshm_open");
        }

        return EXIT_FAILURE;
    }

    /* Truncate the memory segment to the size of a block */
    if (ftruncate(shm_block_fd, sizeof(Block))==-1) {
        perror("Error: could not truncate shared memory size to block structure\nftruncate");
        close(shm_block_fd);
        unlink(SHM_NAME_BLOCK);
        return EXIT_FAILURE;
    }

    /* Map the memory segment. */
    *blockStruct = mmap(NULL, sizeof(NetData), PROT_READ | PROT_WRITE, MAP_SHARED, shm_block_fd, 0);
    close(shm_block_fd);
    if (*blockStruct == MAP_FAILED) {
        perror("Error: could not map block structure\nmmap");
        unlink(SHM_NAME_BLOCK);
        return EXIT_FAILURE;
    }

    (*blockStruct)->id = 1;
    (*blockStruct)->is_valid = FALSE;
    (*blockStruct)->target = (long) ((double)rand()*(double)PRIME/(double)RAND_MAX);
    (*blockStruct)->solution = -1;
    (*blockStruct)->wallets[0] = 0;
    for (int i=1; i<MAX_MINERS; i++) (*blockStruct)->wallets[i] = -1;
    (*blockStruct)->next = NULL;
    (*blockStruct)->prev = NULL;

    return EXIT_SUCCESS;
}

/* Private */
int init_net(NetData *netStruct, Block **blockStruct, int *miner_ind) {

    /* Initialize mutex */
    if (sem_init(&(netStruct->sem_net_mutex), 1, 0) == -1
        || sem_init(&(netStruct->sem_block_mutex), 1, 0) == -1
        || sem_init(&(netStruct->sem_round), 1, 1) == -1
        || sem_init(&(netStruct->sem_winner), 1, 1) == -1
        || sem_init(&(netStruct->sem_updated), 1, 0) == -1
        || sem_init(&(netStruct->sem_entry), 1, 1) == -1)
    {
        perror("Error: could not initialize a semaphore\nsem_init");
        shm_unlink(SHM_NAME_NET);
        munmap(netStruct, sizeof(NetData));
        return EXIT_FAILURE;
    }

    netStruct->miners_pid[0] = getpid();
    /* Initialize all array elements to -1, to indicate there is no active miner */
    for (int i=1; i<MAX_MINERS; i++) netStruct->miners_pid[i] = -1;
    netStruct->total_miners = 1;
    /* Initialize all array elements to -1, to indicate it is not a valid vote */
    for (int i=0; i<MAX_MINERS; i++) netStruct->voting_pool[i] = -1;
    *miner_ind = 0;

    /* Now other processes can access net data structure on shared memory when joining
     * the net, but block structure on shared memory is still restricted */
    sem_post(&(netStruct->sem_net_mutex));

    if (init_shm_block(blockStruct) == EXIT_FAILURE) {
        shm_unlink(SHM_NAME_NET);
        munmap(netStruct, sizeof(NetData));
        return EXIT_FAILURE;
    }

    /* Now processes joining the net can also open and access block structure on shared memory */
    sem_post(&(netStruct->sem_block_mutex));

    return EXIT_SUCCESS;
}

/* Private */
int open_shm_block(Block **blockStruct, int miner_ind) {
    int shm_block_fd;

    if ((shm_block_fd = shm_open(SHM_NAME_BLOCK, O_RDWR, 0)) == -1) {
        perror("Error: could not open existing block structure on shared memory\nshm_open");
        return EXIT_FAILURE;
    }

    /* Map the memory segment. */
    *blockStruct = mmap(NULL, sizeof(Block), PROT_READ | PROT_WRITE, MAP_SHARED, shm_block_fd, 0);
    close(shm_block_fd);
    if (*blockStruct == MAP_FAILED) {
        perror("Error: could not map block structure\nmmap");
        return EXIT_FAILURE;
    }

    (*blockStruct)->wallets[miner_ind] = 0;

    return EXIT_SUCCESS;
}

/* Private */
int join_net(NetData *netStruct, Block **blockStruct, int *miner_ind) {
    int i;

    sem_wait(&(netStruct->sem_entry));

    sem_wait(&(netStruct->sem_net_mutex));

    for (i=0; netStruct->miners_pid[i]!=-1 && i<netStruct->total_miners+1; i++);
    if (i >= MAX_MINERS) {
        fprintf(stderr, "Error: max miners amount reached\n");
        sem_post(&(netStruct->sem_net_mutex));
        munmap(netStruct, sizeof(NetData));
        return EXIT_FAILURE;
    }
    netStruct->miners_pid[i] = getpid();
    netStruct->total_miners++;
    *miner_ind = i;

    sem_post(&(netStruct->sem_net_mutex));

    sem_wait(&(netStruct->sem_block_mutex));

    if (open_shm_block(blockStruct, *miner_ind) == EXIT_FAILURE) {
        /* If error, revert changes and exit */
        sem_post(&(netStruct->sem_block_mutex));
        netStruct->miners_pid[i] = -1;
        netStruct->total_miners--;
        munmap(netStruct, sizeof(NetData));
        return EXIT_FAILURE;
    }

    sem_post(&(netStruct->sem_block_mutex));

    /* To enter into miner's main loop */
    sem_post(&(netStruct->sem_round));

    sem_post(&(netStruct->sem_entry));

    fprintf(stdout, "Successfuly joined net.\n"); /* REMOVE */

    return EXIT_SUCCESS;
}

int net_register(NetData **netStruct, Block **blockStruct, int *miner_ind) {
    int shm_net_fd;
    int exist = FALSE;

    /* Try creating net info structure on share memory */
    if ((shm_net_fd = shm_open(SHM_NAME_NET, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR)) == -1) {
        if (errno == EEXIST) {
            exist = TRUE;
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
    if (!exist && ftruncate(shm_net_fd, sizeof(NetData))==-1) {
        perror("Error: could not truncate shared memory size to miner net data structure\nftruncate");
        close(shm_net_fd);
        unlink(SHM_NAME_NET);
        return EXIT_FAILURE;
    }

    /* Map the memory segment. */
    *netStruct = mmap(NULL, sizeof(NetData), PROT_READ | PROT_WRITE, MAP_SHARED, shm_net_fd, 0);
    close(shm_net_fd);
    if (*netStruct == MAP_FAILED) {
        perror("Error: could not map miner net info structure\nmmap");
        if (!exist) unlink(SHM_NAME_NET); /* If I am the first miner, delete the shared memory */
        return EXIT_FAILURE;
    }

    if (!exist) return init_net(*netStruct, blockStruct, miner_ind);
    else return join_net(*netStruct, blockStruct, miner_ind);
}




/**********************
 ** Mining functions **
 **********************/

long int simple_hash(long int number) {
    long int result = (number * BIG_X + BIG_Y) % PRIME;
    return result;
}

void *worker_main_loop(struct worker_args_struct *args) {
    long i;

    for (i=args->start; flag && i<args->end; i++) {
        if (args->target == simple_hash(i)) {
            flag = 0;
            args->solution = i;
            pthread_exit(NULL);
        }
    }

    args->solution = -1;
    pthread_exit(NULL);
}

int miner_main_loop(NetData *netStruct, Block *blockStruct, Block **pplast_block, int miner_ind, pthread_t *workers, int n_workers, int n_rounds, int *win) {
    int i, j, k, f, error;
    long target, solution;
    double div;
    struct worker_args_struct *w_args;
    void *ret;

    div = (double)PRIME/(double)n_workers;

    w_args = (struct worker_args_struct *) malloc(n_workers*sizeof(struct worker_args_struct));
    if (w_args == NULL) {
        perror("Error: could not alloc memory\nmalloc");
        return EXIT_FAILURE;
    }

    /* Main loop */
    i = 0; /* Round counter */
    f = FALSE; /* Error handling */
    while (active && (n_rounds<=0 || i<n_rounds)) {
        *win = FALSE;
        sem_wait(&(netStruct->sem_round));

        sem_wait(&(netStruct->sem_block_mutex));
        target = blockStruct->target;
        sem_post(&(netStruct->sem_block_mutex));

        fprintf(stdout, "Searching solution for block with target: %ld\n", target); /* REMOVE */

        /* Initialize workers (threads) */
        for (j=0; flag && j<n_workers; j++) {
            w_args[j].start = j*div;
            w_args[j].end = (j+1)*div;
            w_args[j].target = target;
            w_args[j].solution = -1;
            error = pthread_create(workers+j, NULL, (void*) worker_main_loop, w_args+j);
            if (error != 0) {
                fprintf(stderr, "Error: could not start worker\npthread_create: %s\n", strerror(error));
                flag = FALSE; /* Tell other workers to end */
                f = TRUE; /* To return error later */
                break; /* Stop creating new threads, and join the ones created */
            }
        }

        /* Wait for workers to end */
        for (j--; j>=0; j--) {
            error = pthread_join(workers[j], NULL);
            /* If thread did not end correctly, tell other threads to end, wait for them and return error */
            if (error != 0) {
                fprintf(stderr, "Error: worker did not end correctly\npthread_join: %s\n", strerror(error));
                flag = FALSE; /* Tell other active threads to end */
                f = TRUE; /* To return error later */
            }
            else if (!f && w_args[j].solution > 0) {
                /* If worker found the solution, and no error occured, update win status */
                solution = w_args[j].solution;
                *win = TRUE;
            }
        }

        /* Return error if a thread could not be created, or could not end correctly */
        if (f) return EXIT_FAILURE;

        if (*win == TRUE) {
            if (sem_trywait(&(netStruct->sem_winner)) == 0) {

                fprintf(stdout, "Found solution: %ld for block with target: %ld\n", solution, target); /* REMOVE */

                /* Now other miners must wait to enter the net */
                sem_wait(&(netStruct->sem_entry));

                /* Stop other miners */
                sem_wait(&(netStruct->sem_net_mutex));
                for (k=0, k=0; j < netStruct->total_miners && k < MAX_MINERS; k++) {
                    if (netStruct->miners_pid[k] != -1 && netStruct->miners_pid[k] != getpid()) {
                        kill(netStruct->miners_pid[k], SIGUSR2);
                        j++;
                    }
                }
                sem_post(&(netStruct->sem_net_mutex));

                sem_wait(&(netStruct->sem_block_mutex));
                /* Write solution to shared memory block */
                blockStruct->solution = solution;

                /* TODO Handle voting */
                blockStruct->is_valid = TRUE; /* Do if successful voting */
                blockStruct->wallets[miner_ind]++; /* Do if successful voting */

                sem_post(&(netStruct->sem_block_mutex));
                sem_post(&(netStruct->sem_winner));
            }
            else {
                /* Another miner has already won */
                if (error == EAGAIN) *win = FALSE;
            }
        }
        else {
            fprintf(stdout, "Mining round finished without success.\n"); /* REMOVE */
            /* TODO Vote */
        }

        /* TODO Check if valid before */
        sem_wait(&(netStruct->sem_block_mutex));
        if (blockStruct->is_valid) {
            /* sem_post(&(netStruct->sem_block_mutex)); */
            sem_wait(&(netStruct->sem_winner));
            /* sem_wait(&(netStruct->sem_block_mutex)); */
            fprintf(stdout, "Updating blockchain with winner's solution.\n"); /* REMOVE */
            if (update_blockchain(pplast_block, blockStruct) != EXIT_SUCCESS) {
                /* CdE */
            }
            sem_post(&(netStruct->sem_block_mutex));
            sem_post(&(netStruct->sem_winner));

            /* For the next round */
            i++;
            flag = TRUE;
            if (*win) {
                /* Winner does not have to do UP on update sem */
                prepare_next_round(netStruct, blockStruct);
            }
            else if (active && (n_rounds<=0 || i<n_rounds)) {
                /* If this is miner's last round, update just after
                 * cleaning and decreasing total miners, so it will
                 * not be active when the next round begins. */
                sem_post(&(netStruct->sem_updated));
            }
        }
        else {
            sem_post(&(netStruct->sem_block_mutex));
            sem_post(&(netStruct->sem_round));
        }
    }

    free(w_args);

    return EXIT_SUCCESS;
}

int prepare_next_round(NetData *netStruct, Block *blockStruct) {
    int i, n_miners;

    sem_wait(&(netStruct->sem_net_mutex));
    n_miners = netStruct->total_miners;
    sem_post(&(netStruct->sem_net_mutex));

    /* Wait for all miners (except winner) to update their blockchains */
    for (i=0; i<n_miners-1; i++) {
        sem_wait(&(netStruct->sem_updated));
    }

    sem_wait(&(netStruct->sem_block_mutex));

    blockStruct->id++ ;
    blockStruct->target = blockStruct->solution;
    blockStruct->solution = -1;
    blockStruct->is_valid = FALSE;

    sem_post(&(netStruct->sem_block_mutex));

    /* TODO:
     * Asegurarse que llegados a este punto, el numero total de
     * mineros activos esté actualizado con los que se han ido
     * y con los que han entrado, y que todos los mineros han
     * actualizado su blockchain. */

    for (i=0; i<n_miners; i++) {
        sem_post(&(netStruct->sem_round));
    }

    /* Now other miners can join the net */
    sem_post(&(netStruct->sem_entry));

    return EXIT_SUCCESS;
}

int update_blockchain(Block **pplast_block, Block *shm_block) {
    Block *temp;

    temp = (Block*) malloc(sizeof(Block));
    if (memcpy(temp, shm_block, sizeof(Block)) == NULL) return EXIT_FAILURE;

    temp->prev = *pplast_block;
    if (*pplast_block != NULL) (*pplast_block)->next = temp;

    *pplast_block = temp;

    return EXIT_SUCCESS;
}

void print_blocks(Block *plast_block) {
    Block *block = NULL;
    int i, j;

    for(i = 0, block = plast_block; block != NULL; block = block->prev, i++) {
        printf("Block number: %d; Target: %ld;    Solution: %ld\n", block->id, block->target, block->solution);
        for(j = 0; j < MAX_MINERS; j++) {
            if (block->wallets[j] != -1) printf("%d: %d;         ", j, block->wallets[j]);
        }
        printf("\n\n\n");
    }
    printf("A total of %d blocks were printed\n", i);
}

int clean(NetData *netStruct, Block *blockStruct, Block *plast_block, int miner_ind, int *win) {

    sem_wait(&(netStruct->sem_block_mutex));

    blockStruct->wallets[miner_ind] = -1;

    sem_post(&(netStruct->sem_block_mutex));

    sem_wait(&(netStruct->sem_net_mutex));

    /* Tell other miners this miner is finished */
    netStruct->miners_pid[miner_ind] = -1;
    netStruct->total_miners--;

    if (netStruct->total_miners <= 0) {

        fprintf(stdout, "Last miner, destroying net.\n"); /* REMOVE */

        /* Free shared memory if this is the last miner */
        sem_destroy(&netStruct->sem_net_mutex);
        sem_destroy(&netStruct->sem_block_mutex);
        sem_destroy(&netStruct->sem_round);
        sem_destroy(&netStruct->sem_winner);
        sem_destroy(&netStruct->sem_updated);
        sem_destroy(&netStruct->sem_entry);
        shm_unlink(SHM_NAME_NET);
        shm_unlink(SHM_NAME_BLOCK);
    } else {
        sem_post(&(netStruct->sem_net_mutex));
        if (!*win) sem_post(&(netStruct->sem_updated));

        fprintf(stdout, "Miner ended successfuly\n"); /* REMOVE */
    }

    munmap(netStruct, sizeof(NetData));
    munmap(blockStruct, sizeof(Block));

    for (Block *block = plast_block, *temp = NULL; block != NULL && temp != NULL; block = temp) {
        temp = block->prev;
        free(block);
    }

    return EXIT_SUCCESS;
}
