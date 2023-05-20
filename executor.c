#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/stat.h> // For mode constants.
#include <fcntl.h>    // For O_* constants.
#include <time.h>
#include <sys/mman.h>

#include "err.h"
#include "utils.h"

struct Storage
{
    int currentFree;
    pid_t pidNumberProgram[4096];
    pid_t pidNumberBoss[4096];
    int statusNumber[4096];
    int pipe_dsc[2];
    sem_t mutex;
    sem_t mutex2;
    sem_t printSem;
    sem_t mutexStart;
    char lastLine[4096][1024];
    char lastLineErr[4096][1024];
    int ileCzeka;
};

void execProgram(char **program, struct Storage *s)
{
    pid_t pid;
    ASSERT_SYS_OK(pid = fork());
    if (!pid){
        // Proces zajmujacy się exec, out, err
        pid_t pid2;
        int pipe_dscOut[2];
        int pipe_dscErr[2];
        ASSERT_SYS_OK(pipe(pipe_dscOut));
        ASSERT_SYS_OK(pipe(pipe_dscErr));
        for (int i = 0; i < 2; i++){
            set_close_on_exec(pipe_dscErr[i], true);
            set_close_on_exec(pipe_dscOut[i], true);
        }
        sem_wait(&(s->mutex2));
        int number = s->currentFree;

        ASSERT_SYS_OK(pid2 = fork());

        if (!pid2){
            // proces exec
            const char *filename = program[0];
            
            printf("Task %d started: pid %d.\n", number, getpid());
            fflush(stdout);

            ASSERT_SYS_OK(close(pipe_dscOut[0]));
            ASSERT_SYS_OK(dup2(pipe_dscOut[1], STDOUT_FILENO));
            ASSERT_SYS_OK(close(pipe_dscOut[1]));

            ASSERT_SYS_OK(close(pipe_dscErr[0]));
            ASSERT_SYS_OK(dup2(pipe_dscErr[1], STDERR_FILENO));
            ASSERT_SYS_OK(close(pipe_dscErr[1]));

            ASSERT_SYS_OK(execvp(filename, program));
        }
        else{
            FILE *fileOut = fdopen(pipe_dscOut[0], "r");
            pid_t pidOut = fork();
            if (!pidOut){
                // proces out
                char temp[1024];
                while (read_line(temp, 1024, fileOut)){

                    int ite = 0;
                    for (int i = 0;; i++){
                        ite++;
                        if (temp[i] == '\n')
                            temp[i] = '\0';
                        if (temp[i] == '\0')
                            break;
                    }
                    
                    sem_wait(&(s->mutex2));
                    for (int i = 0; i < ite; i++)
                        s->lastLine[number][i] = temp[i];
                    sem_post(&(s->mutex2));
                }
                fclose(fileOut);
                exit(0);
            }

            FILE *fileErr = fdopen(pipe_dscErr[0], "r");
            pid_t pidErr = fork();
            if (!pidErr){
                // proces err
                char temp[1024];
                while (read_line(temp, 1024, fileErr)){

                    int ite = 0;
                    for (int i = 0;; i++)
                    {
                        ite++;
                        if (temp[i] == '\n')
                            temp[i] = '\0';
                        if (temp[i] == '\0')
                            break;
                    }

                    sem_wait(&(s->mutex2));
                    for (int i = 0; i < ite; i++)
                        s->lastLineErr[number][i] = temp[i];
                    sem_post(&(s->mutex2));
                }
                fclose(fileErr);
                exit(0);
            }

            s->pidNumberProgram[number] = pid2;
            ASSERT_SYS_OK(close(pipe_dscErr[1]));
            ASSERT_SYS_OK(close(pipe_dscOut[1]));

            void out_handler(int sig)
            {
                assert(sig == SIGUSR1);
                sem_wait(&(s->mutex2));
                printf("Task %d stdout: '%s'.\n", number, s->lastLine[number]);
                sem_post(&(s->mutex2));
            }

            void err_handler(int sig)
            {
                assert(sig == SIGUSR2);
                sem_wait(&(s->mutex2));
                printf("Task %d stderr: '%s'.\n", number, s->lastLineErr[number]);
                sem_post(&(s->mutex2));
            }

            void kill_handler(int sig)
            {
                assert(sig == SIGINT);
                kill(pid2, SIGINT);
                wait(NULL);
                kill(pidErr, SIGINT);
                kill(pidOut, SIGINT);
                fclose(fileOut);
                fclose(fileErr);
                exit(0);
            }

            signal(SIGUSR1, out_handler);
            signal(SIGINT, kill_handler);
            signal(SIGUSR2, err_handler);

            sem_post(&(s->mutex));
            sem_post(&(s->mutex2));

            int status;
            wait(&status);
            fclose(fileOut);
            fclose(fileErr);
            usleep(1000);
            kill(pidErr, SIGINT);
            kill(pidOut, SIGINT);
            wait(NULL);
            wait(NULL);
            
            sem_wait(&(s->mutex2));
            s->ileCzeka++;
            sem_post(&(s->mutex2));

            sem_wait(&(s->printSem));
            sem_wait(&(s->mutex2));

            s->statusNumber[number] = -1;
            if (WIFEXITED(status)){
                printf("Task %d ended: status %d.\n", number, WEXITSTATUS(status));
                fflush(stdout);
            }
            else{
                printf("Task %d ended: signalled.\n", number);
                fflush(stdout);
            }
            s->ileCzeka--;
            if(s->ileCzeka == 0){
                sem_post(&(s->mutexStart));
            }
            else{
                sem_post(&(s->printSem));
            }
            sem_post(&(s->mutex2));
            exit(0);
        }
    }
    else
    {
        sem_wait(&(s->mutex));
        s->pidNumberBoss[s->currentFree] = pid;
        s->statusNumber[s->currentFree] = 1;
        s->currentFree++;
    }
}

void killProcces(int id, struct Storage *s)
{
    sem_wait(&(s->mutex2));
    if (s->statusNumber[id] == 1)
    {
        pid_t pid = s->pidNumberBoss[id];
        s->statusNumber[id] = -1;
        kill(pid, SIGINT);
        printf("Task %d ended: signalled.\n", id);
        fflush(stdout);
    }
    sem_post(&(s->mutex2));
}

int main()
{
    char *inputLine = malloc(512 * sizeof(char));
    size_t intputLineSize = 512;

    struct Storage *storage = mmap(
        NULL,
        sizeof(struct Storage),
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS,
        -1,
        0);

    sem_init(&storage->mutex, 1, 0);
    sem_init(&storage->printSem, 1, 0);
    sem_init(&storage->mutex2, 1, 1);
    sem_init(&storage->mutexStart, 1, 0);


    while (read_line(inputLine, intputLineSize, stdin)){
        for(int i = 0 ; i < storage->currentFree ; i++){
            if(storage->statusNumber[i] == -1){
                storage->statusNumber[i] = 0;
                wait(NULL);
            }
        }

        // MODYFIKACJA WEJSCIA / PUSTE WEJSCIE
        char **strings = split_string(inputLine);
        for (int i = 0;; i++){
            if (strings[i] == NULL)
                break;
            else{
                for (int j = 0;; j++){
                    if (strings[i][j] == '\n')
                        strings[i][j] = '\0';
                    if (strings[i][j] == '\0')
                        break;
                }
            }
        }
        if (strings == NULL || strings[0] == NULL || strings[0][0] == '\0' || strings[0][0] == '\n'){
            free_split_string(strings);
            sem_post(&(storage->printSem));
            continue;
        }

        const char *command = strings[0];
        if (!strcmp(command, "run"))
            execProgram(&strings[1], storage);
        else if (!strcmp(command, "out")){
            int number = atoi(strings[1]);
            if (storage->statusNumber[number] == 1)
                kill(storage->pidNumberBoss[number], SIGUSR1);
            else{
                printf("Task %d stdout: '%s'.\n", number, storage->lastLine[number]);
                fflush(stdout);
            }
        }
        else if (!strcmp(command, "err")){
            int number = atoi(strings[1]);
            if (storage->statusNumber[number] == 1)
                kill(storage->pidNumberBoss[number], SIGUSR2);
            else
            {
                printf("Task %d stderr: '%s'.\n", number, storage->lastLineErr[number]);
                fflush(stdout);
            }
        }
        else if (!strcmp(command, "kill"))
            killProcces(atoi(strings[1]), storage);
        else if (!strcmp(command, "sleep")){
            int time = atoi(strings[1]);
            usleep(time * 1000);
        }
        else if (!strcmp(command, "quit")){
            for (int i = 0; i < storage->currentFree; i++){
                killProcces(i, storage);
            }
            free_split_string(strings);
            free(inputLine);
            sem_destroy(&(storage->mutex2));
            sem_destroy(&(storage->mutex));
            munmap(storage, sizeof(struct Storage));
            exit(0);
        }
        else{
            printf("Unknown command!\n");
            fflush(stdout);
        }
        free_split_string(strings);

        sem_wait(&(storage->mutex2));

        if(storage->ileCzeka > 0){
            sem_post(&(storage->printSem));
            sem_post(&(storage->mutex2));
            sem_wait(&(storage->mutexStart));
        }else{
            sem_post(&(storage->mutex2));
        }


    }
    free(inputLine);
    sem_destroy(&(storage->mutex2));
    sem_destroy(&(storage->mutex));
    sem_destroy(&(storage->printSem));
    munmap(storage, sizeof(struct Storage));
    exit(0);
}