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
#include <glob.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define MAX_NUM_CONNECTIONS 10
#define MAX_QUEUE_SIZE 10
#define PORT 5798

/* Structure à envoyer sur le port 9988 pour informer d'un changement d'état de connexion ou de file d'attente. */
typedef struct monitoringMessage {
    /* Possible types : HELLO, BYE, INFO. */
    char type[6];
    int sizeLeft;
    int workerIndex;
} monitoringMessage;

/* Structure d'en-tête pour notifier de la taille des informations transitant. */
typedef struct messageHeader {
    int messageSize;
    int priority;
    int taskID;
    char model[64];
    /* 0 -> Utilisation d'un modèle existant,
    *  1 -> Soumission d'un nouveau modèle,
    *  2 -> Erreur de compilation/exécution worker,
    *  3 -> Aucune carte n'est disponible pour prendre la tâche,
    *  4 -> Modèle non existant. */
    int action;
} messageHeader;

/* Booléen pour la terminaison du processus. */
volatile sig_atomic_t isRunning = 1;

/* Socket pour l'envoi de message de monitoring. */
int monitoringSocket;
/* Socket pour la réception des données et l'envoi de résultats. */
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


int getAndWriteFile(struct messageHeader header, char *prefix, char *fileName, char *suffix) {
    char *fileToGet = malloc(header.messageSize);
    if (receiveMessage(receivingSocket, fileToGet, header.messageSize) == -1) {
        printf("ERREUR : Le fichier à traiter n'a pas pu être reçu.\n");
        return -1;
    }
    printf("Réception du fichier à traiter réussie.\n");

    /* 5. Ecrire le fichier dans la mémoire de la jetson. */
    char filePath[128];
    sprintf(filePath, "%s%s%s", prefix, fileName, suffix);
    int taskFd = open(filePath, O_CREAT | O_WRONLY | O_EXCL, 0666);

    if (taskFd == -1) {
        printf("ERREUR : Le fichier n'a pas pu être créé ou existe déjà : %s.\n", filePath);
        return -1;
    }
    printf("Ecriture du fichier à traiter à l'emplacement : %s.\n", filePath);

    int writtenBytes = 0;
    int bytesToWrite = header.messageSize;
    while (writtenBytes < (int) (header.messageSize)) {
        int bytesWritten = write(taskFd, fileToGet + writtenBytes, bytesToWrite);
        if (bytesWritten == -1) {
            printf("ERREUR : L'écriture du fichier %s s'est mal déroulée", filePath);
            return -1;
        }
        if (bytesWritten == 0) {
            break;
        }
        writtenBytes += bytesWritten;
        bytesToWrite -= bytesWritten;
    }
    printf("Ecriture du fichier à traiter %s terminée.\n", filePath);
    free(fileToGet);
    close(taskFd);
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
        printf("Réception du header réussie..\n");

        if (header.action == 0) {
            char fileName[4];
            sprintf(fileName, "%c", header.taskID + '0');
            getAndWriteFile(header, "", fileName, "");
        } else if (header.action == 1) {
            /* Sauvegarder un nouveau modèle. */
            /* On récupère le fichier .onnx. */
            if (getAndWriteFile(header, "./data/", header.model, ".onnx") == -1) {
                header.action = 2;
                sendMessage(receivingSocket, (const char *) &header, sizeof(header));
            }

            /* On récupère le second header. */
            if (receiveMessage(receivingSocket, &header, sizeof(header)) == -1) {
                printf("ERREUR : Le second header n'a pas pu être reçu.\n");
                return NULL;
            }
            printf("Réception du second header réussie.\n");

            /* On récupère le fichier .cpp. */
            if (getAndWriteFile(header, "./inferenceFiles/", header.model, ".cpp") == -1) {
                header.action = 2;
                sendMessage(receivingSocket, (const char *) &header, sizeof(header));
            }
        } else if (header.action == 5) {
            /* Si l'orchestrateur s'est éteint. */
            printf("Fermeture de l'orchestrateur enregistrée, terminaison du programme.\n");
            exit(EXIT_SUCCESS);
        }

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
    }

    close(monitoringSocket);
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

    int opt = 1;
    if (setsockopt(communicationSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        printf("ERREUR : Impossible d'appliquer SO_REUSEADDR.\n");
        exit(EXIT_FAILURE);
    }

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

        /* 8. Récupérer la première tâche dans la file d'attente. */
        struct messageHeader currentTask = taskQueue[0];
        int currentTaskID = currentTask.taskID;

        for (int i = 0; i < MAX_QUEUE_SIZE - 1; i++) {
            taskQueue[i] = taskQueue[i + 1];
        }
        numTasksWaiting--;
        sendMonitoringMessage(MAX_QUEUE_SIZE - numTasksWaiting, "INFO");
        
        pthread_mutex_unlock(&varMutex);
        
        char resultName[32];
        char filePath[32];
        if (currentTask.action == 0) {
            sprintf(filePath, "%d", currentTaskID);
            
            pid_t pid;

            /* 10. L'exécuter et pipe sa sortie dans un fichier résultat. */
            char command[2048];
            sprintf(command, "./models/%s", currentTask.model);
            sprintf(resultName, "%d.txt", currentTaskID);

            if ((pid = fork()) == -1) {
                printf("ERREUR : Le fork n'a pas fonctionné.\n");
                continue;
            }

            if (pid == 0) {
                /* Dans le fils, on exécute le bon modèle et on branche la sortie standard sur le fichier de résultats. */
                int resultFd = open(resultName, O_CREAT | O_WRONLY | O_TRUNC, 0666);
                if (resultFd == -1) {
                    printf("ERREUR : Le fichier de résultat n'a pas pu être ouvert.\n");
                    continue;
                }
                printf("Création du fichier de résultat réussie.\n");
                printf("Lancement de l'exécutable.\n");

                dup2(resultFd, STDOUT_FILENO);
                close(resultFd);

                char *argList[3];
                argList[0] = command;
                argList[1] = filePath;
                argList[2] = NULL;

                execvp(command, argList);
                exit(EXIT_FAILURE);
            } else {
                /* Dans le père, on attends la fin de l'exécution du fils. */
                int status;
                waitpid(pid, &status, 0);

                if (!WIFEXITED(status)) {
                    printf("ERREUR.\n");
                    struct messageHeader header;
                    memset(&header, 0, sizeof(header));
                    header.action = 2;
                    header.taskID = currentTaskID;
                    sendMessage(receivingSocket, (const char *) &header, sizeof(header));
                    remove(resultName);
                    continue;
                }

                if (WEXITSTATUS(status) != 0) {
                    printf("ERREUR : Le lancement s'est mal déroulé.\n");
                    struct messageHeader header;
                    memset(&header, 0, sizeof(header));
                    header.action = 2;
                    header.taskID = currentTaskID;
                    sendMessage(receivingSocket, (const char *) &header, sizeof(header));
                    remove(resultName);
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
            memset(header.model, 0, sizeof(header.model));

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
            remove(resultName);
            remove(filePath);
            fclose(resultFile);
            free(fileToSend);
        } else if (currentTask.action == 1) {
            /* Lancement de la compilation avec g++. */
            pid_t pid;

            if ((pid = fork()) == -1) {
                printf("ERREUR : Le fork n'a pas fonctionné.\n");
                continue;
            }

            if (pid == 0) {
                /* Dans le fils, on compile le fichier .cpp. */
                printf("Lancement de la compilation g++.\n");

                int argIndex = 0;

                char outPath[128];
                sprintf(outPath, "./models/%s", currentTask.model);

                char inPath[128];
                sprintf(inPath, "./inferenceFiles/%s.cpp", currentTask.model);

                glob_t glob_resultats;
    
                int retourGlob1 = glob("/usr/src/tensorrt/samples/common/*.cpp", 0, NULL, &glob_resultats);
                
                int retourGlob2 = glob("/usr/src/tensorrt/samples/utils/*.cpp", GLOB_APPEND, NULL, &glob_resultats);
                
                if (retourGlob1 != 0 || retourGlob2 != 0) {
                    printf("Erreur lors de la résolution des fichiers sources NVIDIA.\n");
                    exit(EXIT_FAILURE);
                }

                char *argList[128];
                argList[argIndex++] = "g++";
                argList[argIndex++] = "-o";
                argList[argIndex++] = outPath;
                argList[argIndex++] = inPath;
                for (size_t i = 0; i < glob_resultats.gl_pathc; i++) {
                    argList[argIndex++] = glob_resultats.gl_pathv[i];
                }
                argList[argIndex++] = "-I/usr/src/tensorrt/samples/common";
                argList[argIndex++] = "-I/usr/local/cuda/include";
                argList[argIndex++] = "-I/usr/src/tensorrt/samples";
                argList[argIndex++] = "-Wno-deprecated-declarations";
                argList[argIndex++] = "-L/usr/local/cuda/lib64";
                argList[argIndex++] = "-lnvinfer";
                argList[argIndex++] = "-lnvonnxparser";
                argList[argIndex++] = "-lcudart";
                argList[argIndex++] = NULL;

                execvp("g++", argList);
                exit(EXIT_FAILURE);
            } else {
                /* Dans le père, on attends la fin de l'exécution du fils. */
                int status;
                waitpid(pid, &status, 0);

                if (!WIFEXITED(status)) {
                    printf("ERREUR.\n");
                    struct messageHeader header;
                    memset(&header, 0, sizeof(header));
                    header.action = 2;
                    header.taskID = currentTaskID;
                    sendMessage(receivingSocket, (const char *) &header, sizeof(header));

                    char onnxFileName[128];
                    sprintf(onnxFileName, "./data/%s.onnx", header.model);
                    char inferenceFileName[128];
                    sprintf(inferenceFileName, "./inferenceFiles/%s.cpp", header.model);

                    remove(inferenceFileName);
                    remove(onnxFileName);
                    continue;
                }

                if (WEXITSTATUS(status) != 0) {
                    printf("ERREUR : La compilation s'est mal déroulée.\n");
                    struct messageHeader header;
                    memset(&header, 0, sizeof(header));
                    header.action = 2;
                    header.taskID = currentTaskID;
                    sendMessage(receivingSocket, (const char *) &header, sizeof(header));

                    char onnxFileName[128];
                    sprintf(onnxFileName, "./data/%s.onnx", header.model);
                    char inferenceFileName[128];
                    sprintf(inferenceFileName, "./inferenceFiles/%s.cpp", header.model);

                    remove(inferenceFileName);
                    remove(onnxFileName);
                    continue;
                }
                printf("Compilation terminée avec succès.\n");

                if (sendMessage(receivingSocket, (const char *) &currentTask, sizeof(currentTask)) == -1) {
                    printf("ERREUR : L'envoi du header au client à échoué.\n");
                    continue;
                }
                printf("Envoi du header terminé.\n");
            }
        }
    }

    close(communicationSocket);
    close(receivingSocket);
    return 0;
}
