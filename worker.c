#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define MAX_NUM_CONNECTIONS 10
#define MAX_QUEUE_SIZE 10
#define PORT 5798

/* Structure d'en-tête pour notifier de la taille des informations transitant. */
typedef struct messageHeader {
    int messageSize;
    int priority;
    int taskID;
} messageHeader;

/* Structure à envoyer sur le port 9988 pour informer d'un changement d'état de connexion ou de file d'attente. */
typedef struct monitoringMessage {
    /* Possible types : HELLO, BYE, INFO. */
    char type[6];
    int sizeLeft;
    int workerIndex;
} monitoringMessage;

/* Booléen pour la terminaison du processus. */
volatile sig_atomic_t isRunning = 1;

/* Socket pour l'envoi de message de monitoring. */
int monitoringSocket;
/* Socket pour la réception de fichier .cu et l'envoi de résultats. */
int communicationSocket;
/* Socket pour les envois. */
int receivingSocket;

/* Variable condition permettant de reprendre le thread de calcul. */
pthread_cond_t varCondition;
/* Verrou associé à la variable condition. */
pthread_mutex_t varMutex;

/* Tableau de tâches. */
messageHeader taskQueue[MAX_QUEUE_SIZE];
/* Nombre de tâches en attente. */
int numTasksWaiting = 0;

/* Nom de la worker. */
char hostname[1024];
/* Index de la carte. */
int workerIndex;


/** Méthode helper afin d'envoyer un bloc d'information dans le socket clientSocket.
 * @param clientSocket Le socket depuis lequel envoyer les informations
 * @param messageToSend Le message à envoyer
 * @param size La taille du message à envoyer
 * @return -1 si l'envoi à échoué, 0 si l'envoi s'est bien déroulé
 */
int sendMessage(int clientSocket, const char *messageToSend, int size) {
    int sentBytes = 0;
    int bytesToSend = size;
    while (sentBytes < size) {
        int bytesSent = (send(clientSocket, messageToSend + sentBytes, bytesToSend, MSG_NOSIGNAL));
        if (bytesSent == -1) {
            return -1;
        }
        sentBytes += bytesSent;
        bytesToSend -= bytesSent;
    }
    return 0;
}


int sendMonitoringMessage(int sizeLeft, char *type) {
    struct monitoringMessage message;
    message.sizeLeft = sizeLeft;
    strcpy(message.type, type);
    message.workerIndex = workerIndex;

    if (sendMessage(monitoringSocket, (const char *) &message, sizeof(message)) == -1) {
        printf("ERREUR : Le message de monitoring %s n'a pas pu être envoyé.\n", type);
        return -1;
    }
    printf("Confirmation de l'envoi du message de %s.\n", type);
    return 0;
}


void sigIntHandler(int sig) {
    (void) sig;

    sendMonitoringMessage(0, "BYE");
    pthread_mutex_lock(&varMutex);
    pthread_cond_signal(&varCondition);
    pthread_mutex_unlock(&varMutex);
    isRunning = 0;
}


void initInteruptHandling() {
    /* Récupération du signal SIGINT pour fermer proprement le socket. */
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = sigIntHandler;
    sigaction(SIGINT, &action, NULL);
}


/** Méthode helper afin de recevoir un bloc d'information depuis le réseau.
 * @param clientSocketFd Le file descriptor du socket vers lequel sont envoyées les informations
 * @param messageToReceive Le message à recevoir du réseau
 * @param size La taille du message à recevoir
 * @return -1 si l'envoi à échoué, 0 si l'envoi s'est bien déroulé
 */
int receiveMessage(int clientSocketFd, void *messageToReceive, int size) {
    int receivedBytes = 0;
    int bytesToReceive = size;
    int bytesLeft = bytesToReceive;
    while (receivedBytes < bytesToReceive) {
        int bytesReceived = (recv(clientSocketFd, (char*) messageToReceive + receivedBytes, bytesLeft, 0));
        if (bytesReceived <= 0) {
            return -1;
        }
        receivedBytes += bytesReceived;
        bytesLeft -= bytesReceived;
    }
    return 0;
}


int compareTasks(const void* _task1, const void* _task2) {
    struct messageHeader *task1 = (messageHeader *) _task1;
    struct messageHeader *task2 = (messageHeader *) _task2;

    if (task1->priority > task2->priority) {
        return 1;
    } else if (task1->priority < task2->priority) {
        return -1;
    }

    return 0;
}


void* monitoringMain(void* _arg) {
    (void) _arg;
    /* 2. Créer un socket vers le port 9988 de l'orchestrateur */
    if ((monitoringSocket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        printf("ERREUR : Le socket n'a pas pu être créé.\n");
        return NULL;
    }
    printf("Socket sur le port 9988 de l'orchestrateur créé.\n");

    struct sockaddr_in orchestratorMonitoringAddress;
    memset(&orchestratorMonitoringAddress, 0, sizeof(orchestratorMonitoringAddress));

    orchestratorMonitoringAddress.sin_addr.s_addr = inet_addr("147.127.121.93");
    orchestratorMonitoringAddress.sin_family = AF_INET;
    orchestratorMonitoringAddress.sin_port = htons(9988);

    if (connect(monitoringSocket, (const struct sockaddr *) &orchestratorMonitoringAddress, sizeof(orchestratorMonitoringAddress)) == -1) {
        printf("ERREUR : La connexion au port de monitoring de l'orchestrateur à échoué.\n");
        return NULL;
    }
    printf("Connexion au port 9988 de l'orchestrateur réussie.\n");

    /* 3. Envoyer un message de connexion à l'orchestrateur. */
    if (sendMonitoringMessage(0, "HELLO") == -1) {
        return NULL;
    }

    pthread_mutex_lock(&varMutex);
    if (sendMonitoringMessage(MAX_QUEUE_SIZE - numTasksWaiting, "INFO") == -1) {
        return NULL;
    }
    pthread_mutex_unlock(&varMutex);

    while (isRunning == 1) {
        fd_set socketReadSet;

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int selectReturn = 0;
        while (selectReturn <= 0 && isRunning == 1) {
            FD_ZERO(&socketReadSet);
            FD_SET(receivingSocket, &socketReadSet);
            selectReturn = select(receivingSocket + 1, &socketReadSet, NULL, NULL, &timeout);
        }

        /* 4. Recevoir les headers et fichiers depuis le réseau. */
        struct messageHeader header;
        if (receiveMessage(receivingSocket, &header, sizeof(header)) == -1) {
            printf("ERREUR : Le header n'a pas pu être reçu.\n");
            return NULL;
        }
        printf("Réception du header réussie.\n");

        char *fileToCompile = malloc(header.messageSize * sizeof(char));
        if (receiveMessage(receivingSocket, fileToCompile, header.messageSize * sizeof(char)) == -1) {
            printf("ERREUR : Le fichier .cu n'a pas pu être reçu.\n");
            continue;
        }
        printf("Réception du fichier .cu réussie.\n");

        /* 5. Ecrire le fichier dans la mémoire de la jetson. */
        char filePath[8];
        sprintf(filePath, "%d.cu", header.taskID);
        int taskFd = open(filePath, O_CREAT | O_WRONLY | O_EXCL, 0666);

        if (taskFd == -1) {
            printf("ERREUR : Le fichier n'a pas pu être créé ou existe déjà : %s.\n", filePath);
            continue;
        }
        printf("Ecriture du fichier .cu à l'emplacement : %s.\n", filePath);

        int writtenBytes = 0;
        int bytesToWrite = header.messageSize * sizeof(char);
        while (writtenBytes < (int) (header.messageSize * sizeof(char))) {
            int bytesWritten = write(taskFd, fileToCompile + writtenBytes, bytesToWrite);
            if (bytesWritten == -1) {
                printf("ERREUR : L'écriture du fichier %s s'est mal déroulée", filePath);
                break;
            }
            if (bytesWritten == 0) {
                break;
            }
            writtenBytes += bytesWritten;
            bytesToWrite -= bytesWritten;
        }
        printf("Ecriture du fichier .cu %s terminée.\n", filePath);

        /* 7. Les placer dans la file d'attente puis trier la file d'attente. */
        pthread_mutex_lock(&varMutex);
        taskQueue[numTasksWaiting] = header;
        numTasksWaiting++;
        qsort(taskQueue, numTasksWaiting, sizeof(header), compareTasks);

        printf("Tâche d'ID %d insérée dans la file d'attente.\n", header.taskID);

        /* 6. Envoyer l'état de la file d'attente à chaque nouveau message reçu. */
        sendMonitoringMessage(MAX_QUEUE_SIZE - numTasksWaiting, "INFO");
        pthread_cond_signal(&varCondition);
        pthread_mutex_unlock(&varMutex);

        free(fileToCompile);
        close(taskFd);
    }

    return NULL;
}


/** Fonction principale. */
int main(void) {
    initInteruptHandling();

    gethostname(hostname, 1023);
    workerIndex = atoi(&hostname[(int) strlen(hostname) - 1]);

    pthread_cond_init(&varCondition, NULL);
    pthread_mutex_init(&varMutex, NULL);
    printf("Initialisation de la variable condition et du mutex associé.\n");

    /* Stocker l'id de la tache ET la priorité pour pouvoir trier la file d'attente. */
    memset(taskQueue, 0, sizeof(taskQueue));

    /* 0. Création du socket d'écoute. */
    if ((communicationSocket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        printf("ERREUR : La création du socket de communication à échouée.\n");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in socketAddress;
    socketAddress.sin_family = AF_INET;
    socketAddress.sin_port = htons(PORT);
    socketAddress.sin_addr.s_addr = INADDR_ANY;

    if (bind(communicationSocket, (const struct sockaddr *) &socketAddress, sizeof(socketAddress)) == -1) {
        printf("ERREUR : Le bind du socket à échoué.\n");
        exit(EXIT_FAILURE);
    }

    if (listen(communicationSocket, MAX_NUM_CONNECTIONS) == -1) {
        printf("ERREUR : Le listen n'a pas abouti.\n");
        exit(EXIT_FAILURE);
    }

    /* 1. Créer un thread d'envoi de l'état de la carte. */
    pthread_t monitoringThread;
    if (pthread_create(&monitoringThread, NULL, monitoringMain, NULL) != 0) {
        printf("ERREUR : Le thread de monitoring n'a pas pu être créé.\n");
        exit(EXIT_FAILURE);
    } else {
        pthread_detach(monitoringThread);
        printf("Lancement du thread de monitoring.\n");
    }

    /* Récupérer le socket pour la communication avec l'orchestrateur après l'envoi du HELLO. */
    if ((receivingSocket = accept(communicationSocket, NULL, NULL)) == -1) {
        printf("ERREUR : La connexion à l'orchestrateur à échouée.\n");
        exit(EXIT_FAILURE);
    }
    printf("Connexion à l'orchestrateur réussie.\n");

    /* On écoute constamment pour pouvoir traiter des demandes à la chaine. */
    while(isRunning == 1) {
        pthread_mutex_lock(&varMutex);
        while (numTasksWaiting == 0 && isRunning == 1) {
            pthread_cond_wait(&varCondition, &varMutex);
        }

        /* Sécurité pour sortir proprement lors d'un SIGINT. */
        if (isRunning != 1) {
            break;
        }

        printf("Il y a actuellement %d tâches en attente, lancement des calculs.\n", numTasksWaiting);

        struct messageHeader currentTask = taskQueue[0];
        int currentTaskID = currentTask.taskID;

        for (int i = 0; i < MAX_QUEUE_SIZE - 1; i++) {
            taskQueue[i] = taskQueue[i + 1];
        }
        numTasksWaiting--;
        
        pthread_mutex_unlock(&varMutex);

        /* 8. Récupérer la première tâche dans la file d'attente. */
        char filePath[8];
        sprintf(filePath, "%d.cu", currentTaskID);
        
        char outPath[8];
        sprintf(outPath, "%d.out", currentTaskID);
        printf("Tâche actuelle ID : %d, correspondant au fichier %s\n", currentTaskID, filePath);

        /* 9. Compiler le fichier .cu. */
        pid_t pid;

        if ((pid = fork()) == -1) {
            printf("ERREUR : Le fork n'a pas fonctionné.\n");
            continue;
        }

        if (pid == 0) {
            /* Dans le fils, on compile le fichier .cu avec nvcc. */
            char *argList[5];
            argList[0] = "nvcc";
            argList[1] = filePath;
            argList[2] = "-o";
            argList[3] = outPath;
            argList[4] = NULL;

            printf("Dans le processus de compilation, lancement de nvcc.\n");
            execvp("nvcc", argList);

            printf("ERREUR : La compilation nvcc à échouée.\n");
            exit(EXIT_FAILURE);
        } else {
            /* Dans le père, on attends la fin de l'exécution du fils. */
            int status;
            waitpid(pid, &status, 0);

            if (!WIFEXITED(status)) {
                printf("ERREUR.\n");
                continue;
            }

            if (WEXITSTATUS(status) != 0) {
                printf("ERREUR : nvcc n'a pas pu compiler le fichier.\n");
                continue;
            }
            printf("Compilation terminée avec succès.\n");
        }

        /* 10. L'exécuter et pipe sa sortie dans un fichier résultat. */
        char command[16];
        sprintf(command, "./%s", outPath);
        
        char resultName[8];
        sprintf(resultName, "%d.txt", currentTaskID);

        if ((pid = fork()) == -1) {
            printf("ERREUR : Le fork n'a pas fonctionné.\n");
            continue;
        }

        if (pid == 0) {
            /* Dans le fils, on exécute le .out et on branche la sortie standard sur le fichier de résultats. */
            int resultFd = open(resultName, O_CREAT | O_WRONLY | O_TRUNC, 0666);
            if (resultFd == -1) {
                printf("ERREUR : Le fichier de résultat n'a pas pu être ouvert.\n");
                exit(EXIT_FAILURE);
            }
            printf("Création du fichier de résultat réussie.\n");
            printf("Lancement de l'exécutable.\n");

            dup2(resultFd, STDOUT_FILENO);
            close(resultFd);

            char *argList[2];
            argList[0] = command;
            argList[1] = NULL;

            execvp(command, argList);
            exit(EXIT_FAILURE);
        } else {
            /* Dans le père, on attends la fin de l'exécution du fils. */
            int status;
            waitpid(pid, &status, 0);

            if (!WIFEXITED(status)) {
                printf("ERREUR.\n");
                continue;
            }

            if (WEXITSTATUS(status) != 0) {
                printf("ERREUR : nvcc n'a pas pu compiler le fichier.\n");
                continue;
            }
            printf("Exécutable terminé avec succès.\n");
        }

        /* 11. Envoyer les résultats sur le réseau. */
        FILE *resultFile = fopen(resultName, "rb");
        fseek(resultFile, 0, SEEK_END);
        int fileSize = ftell(resultFile);
        rewind(resultFile);

        char *fileToSend = malloc(fileSize);
        fread(fileToSend, 1, fileSize, resultFile);

        struct messageHeader header;
        header.messageSize = fileSize;
        header.priority = taskQueue[0].priority;
        header.taskID = currentTaskID;

        if (sendMessage(receivingSocket, (const char *) &header, sizeof(header)) == -1) {
            printf("ERREUR : L'envoi du header au client à échoué.\n");
            goto end_loop;
        }
        printf("Envoi du header terminé.\n");

        if (sendMessage(receivingSocket, fileToSend, fileSize) == -1) {
            printf("ERREUR : L'envoi du fichier au client à échoué.\n");
        }
        printf("Envoi du fichier de résultat terminé.\n");

        /* Nettoyer, mettre à jour la file d'attente puis boucler à l'étape 8. */
        end_loop:
        pthread_mutex_lock(&varMutex);
        sendMonitoringMessage(MAX_QUEUE_SIZE - numTasksWaiting, "INFO");
        pthread_mutex_unlock(&varMutex);

        fclose(resultFile);
        free(fileToSend);
        remove(resultName);
        remove(filePath);
        remove(outPath);
    }

    close(communicationSocket);
    close(monitoringSocket);
    return 0;
}
