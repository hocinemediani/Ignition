#include "orchestrator.h"


/* Nombre de cartes connectées sur le réseau. */
int numConnectedCards = 0;
/* Index de la dernière carte connectée. */
int lastCardIndex = 0;

/* Plus haut nombre de modèles connus. */
int numModels = 0;
/* Liste des noms de modèles existants. */
char *validModelNames[100];

/* Indique si l'orchestrateur doit continuer à fonctionner ou non. */
volatile sig_atomic_t isRunning = 1;

/* Premier offset d'adresses IP. */
int destinationOffset = 94;

/* Tableau contenant les places en file d'attente de chaque worker. */
int workerQueues[NUM_CARDS];
/* Tableau représentant l'état des différents workers (1 -> connecté, 0 -> non connecté). */
int connectedCards[NUM_CARDS];

/* Garde en mémoire l'ID de tâche le plus haut. */
int currentTaskID = 0;
/* Fais la correspondance entre socket client et ID de tâche. */
int routingTable[MAX_CONCURRENT_TASKS];

/* Initialisation des threads de traitement clients. */
pthread_t clientHandlerThreads[MAX_NUM_CONNECTION];

/* Initialisation des threads de traitements des cartes. */
pthread_t jetsonListenerThreads[NUM_CARDS];

/* Initialisation des arguments de chaque thread. */
pthread_args args[MAX_NUM_CONNECTION];
/* Initialisation des arguments de chaque thread. */
pthread_args workerArgs[NUM_CARDS];

/* Initialisation du verrou global des files d'attente. */
pthread_mutex_t queueMutex;
/* Initialisation du tableau de verrou pour l'envoi aux workers. */
pthread_mutex_t workerMutexes[NUM_CARDS];
/* Initialisation du verrou pour l'indentificant de tâche. */
pthread_mutex_t idMutex;

/* Tableau de socket pour les clients. */
socket_t socketTable[MAX_NUM_CONNECTION];
/* Tableau de socket pour les workers. */
socket_t workerSocketTable[NUM_CARDS];
/* Tableau de socket pour le monitoring. */
socket_t monitoringSocketTable[NUM_CARDS];


void sigIntHandler(int sig) {
    (void) sig;
    isRunning = 0;
}


void initInteruptHandling() {
    #ifdef _WIN32
        signal(SIGINT, sigIntHandler);
    #else
        /* Récupération du signal SIGINT pour fermer proprement le socket. */
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        act.sa_handler = sigIntHandler;
        sigaction(SIGINT, &action, NULL);
    #endif
}


/** Méthode helper afin de recevoir un bloc d'information depuis le réseau.
 * @param clientSocketFd Le file descriptor du socket vers lequel sont envoyées les informations
 * @param messageToReceive Le message à recevoir du réseau
 * @param size La taille du message à recevoir
 * @return -1 si l'envoi à échoué, 0 si l'envoi s'est bien déroulé
 */
int receiveMessage(socket_t clientSocketFd, void *messageToReceive, int size) {
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


/** Méthode helper afin d'envoyer un bloc d'information dans le socket clientSocket.
 * @param clientSocket Le socket depuis lequel envoyer les informations
 * @param messageToSend Le message à envoyer
 * @param size La taille du message à envoyer
 * @return -1 si l'envoi à échoué, 0 si l'envoi s'est bien déroulé
 */
int sendMessage(socket_t clientSocket, const char *messageToSend, int size) {
    int sentBytes = 0;
    int bytesToSend = size;
    while (sentBytes < size) {
        int bytesSent = (send(clientSocket, messageToSend + sentBytes, bytesToSend, 0));
        if (bytesSent == -1) {
            return -1;
        }
        sentBytes += bytesSent;
        bytesToSend -= bytesSent;
    }
    return 0;
}


/** Passe le socket clientSocket en mode non-bloquant.
 * @param clientSocket Le socket à rendre non-bloquant
 */
void setNonBlocking(socket_t clientSocket) {
    #ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(clientSocket, FIONBIO, &mode);
    #else
        int flags = fcntl(clientSocket, F_GETFL, 0);
        fcntl(clientSocket, F_SETFL, flags | O_NONBLOCK);
    #endif
}


/** Passe le socket clientSocket en mode bloquant.
 * @param clientSocket Le socket à rendre bloquant
 */
void setBlocking(socket_t clientSocket) {
    #ifdef _WIN32
        u_long mode = 0;
        ioctlsocket(clientSocket, FIONBIO, &mode);
    #else
        int flags = fcntl(clientSocket, F_GETFL, 0);
        fcntl(clientSocket, F_SETFL, flags & ~O_NONBLOCK);
    #endif
}


/** Méthode helper afin d'initialiser le socket principal de l'orchestrateur.
 * @param orchestratorSocket Le socket à initialiser
 */
void initializeSocket(socket_t *orchestratorSocket) {
    if ((*orchestratorSocket = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        printf("ERREUR : Le socket n'a pas pu être créé.\n");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in orchestratorAddress;
    memset(&orchestratorAddress, 0, sizeof(orchestratorAddress));
    orchestratorAddress.sin_addr.s_addr = ADDR_ANY;
    orchestratorAddress.sin_family = AF_INET;
    orchestratorAddress.sin_port = htons(PORT);

    if ((bind(*orchestratorSocket, (const struct sockaddr *) &orchestratorAddress, sizeof(orchestratorAddress))) == SOCKET_ERROR) {
        printf("ERREUR : Le bind au port %d n'a pas pu être réalisé.\n", PORT);
        exit(EXIT_FAILURE);
    }

    if (listen(*orchestratorSocket, MAX_NUM_CONNECTION) == -1) {
        printf("ERREUR : Le listen pour le socket à échoué.\n");
        exit(EXIT_FAILURE);
    }
}


/** Ecoute sur le réseau pour récupérer les informations depuis
 * le client auquel le thread est assigné.
 * @param _arg Les arguments utiles au thread
 */
void* clientListening(void* _arg) {
    pthread_args *arg = (pthread_args *) _arg;

    fd_set socketReadSet;

    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    int selectReturn = 0;
    while (selectReturn <= 0) {
        FD_ZERO(&socketReadSet);
        FD_SET(arg->socket, &socketReadSet);
        selectReturn = select(arg->socket + 1, &socketReadSet, NULL, NULL, &timeout);
        if (selectReturn == -1) goto cleanup;
    }
    selectReturn = 0;

    /* Récupérer le header depuis le client. */
    struct messageHeader header;
    if (receiveMessage(arg->socket, &header, sizeof(header)) == -1) {
        printf("ERREUR : La réception du header du client %d s'est mal passée.\n", arg->index);
        CLOSE_SOCKET(arg->socket);
        return NULL;
    }
    printf("Réception du header du client %d.\n", arg->index);

    /* Utilisation de 2 strings (on envoie au maximum 2 fichiers). */
    char *fileString1;
    char *fileString2;

    int fileSize1;
    int fileSize2;

    if (header.action == 0) {
        /* Si le client souhaite utiliser un modèle. */
        /* On vérifie que le modèle est bien un des modèles connus. */
        int validModel = 0;
        for (int i = 0; i < numModels; i++) {
            if (strcmp(validModelNames[i], header.model) == 0) {
                validModel = 1;
                break;
            }
        }

        /* Si il ne l'est pas, on envoie à l'utilisateur la liste des modèles. */
        if (validModel == 0) {
            /* Préparation du string à envoyer au client. */
            int size = 1;
            for (int i = 0; i < numModels; i++) {
                size += strlen(validModelNames[i]) + 1;
            }

            char *messageToSend = malloc(size);
            memset(messageToSend, 0, size);
            for (int i = 0; i < numModels; i++) {
                sprintf(messageToSend + strlen(messageToSend), "%s\n", validModelNames[i]);
            }

            header.messageSize = size;
            header.priority = 0;
            header.taskID = 0;
            header.action = 4;
            memset(header.model, 0, sizeof(header.model));

            if (sendMessage(arg->socket, (const char *) &header, sizeof(header)) == -1) {
                printf("ERREUR : Le header d'information sur les modèles n'a pas pu être envoyé au client.\n");
                goto end_socket;
            }

            if (sendMessage(arg->socket, messageToSend, size) == -1) {
                printf("ERREUR : Le message d'information sur les modèles n'a pas pu être envoyé au client.\n");
                goto end_socket;
            }
            free(messageToSend);
            printf("Message d'information sur les modèles envoyé au client avec succès.\n");

            end_socket:
            CLOSE_SOCKET(arg->socket);
            return NULL;
        }

        /* 5. Récupérer le fichier à traiter depuis le clientSocket. */
        fileString1 = malloc(header.messageSize);
        if (receiveMessage(arg->socket, fileString1, header.messageSize) == -1) {
            printf("ERREUR : La réception du fichier à traiter n'a pas aboutie.\n");
            CLOSE_SOCKET(arg->socket);
            return NULL;
        }
        printf("Réception du fichier à traiter du client %d.\n", arg->index);
    } else if (header.action == 1) {
        /* Si le client souhaite soumettre un nouveau modèle. */
        /* Réceptionner le fichier .onnx. */
        fileString1 = malloc(header.messageSize);
        fileSize1 = header.messageSize;
        if (receiveMessage(arg->socket, fileString1, header.messageSize) == -1) {
            printf("ERREUR : La réception du fichier à traiter n'a pas aboutie.\n");
            CLOSE_SOCKET(arg->socket);
            return NULL;
        }
        printf("Réception du fichier .onnx réalisée avec succès.\n");

        /* Réceptionner le second header. */
        if (receiveMessage(arg->socket, &header, sizeof(header)) == -1) {
            printf("ERREUR : La réception du header du client %d s'est mal passée.\n", arg->index);
            CLOSE_SOCKET(arg->socket);
            return NULL;
        }

        /* Réceptionner le fichier d'inférence. */
        fileString2 = malloc(header.messageSize);
        fileSize2 = header.messageSize;
        if (receiveMessage(arg->socket, fileString2, header.messageSize) == -1) {
            printf("ERREUR : La réception du fichier à traiter n'a pas aboutie.\n");
            CLOSE_SOCKET(arg->socket);
            return NULL;
        }
        printf("Réception du fichier .cpp réalisée avec succès.\n");
    }

    /* 6. Trouver la carte la moins utilisée par exploration de workerQueues. */
    int tries = 1;
    pthread_mutex_lock(&queueMutex);
    try_queues:
    int minQueue = -1;
    int minQueueIndex = -1;

    for (int i = 0; i < NUM_CARDS; i++) {
        if ((workerQueues[i] > minQueue) && (connectedCards[i] == 1)) {
            minQueue = workerQueues[i];
            minQueueIndex = i;
        }
    }

    /* Si toutes les cartes sont prises, on ré-essaye de trouver une carte libre. */
    if (minQueue <= 0) {
        if (tries == 5) {
            printf("ERREUR : Aucune carte ne peut accepter de tâche pour le moment.\n");
            memset(&header, 0, sizeof(header));
            header.action = 3;
            sendMessage(arg->socket, (const char *) &header, sizeof(header));
            CLOSE_SOCKET(arg->socket);
            pthread_mutex_unlock(&queueMutex);
            return NULL;
        }

        tries++;
        goto try_queues;
    }
    printf("La carte la moins remplie est la orin-nano-%d.\n", minQueueIndex);

    /* L'exploration est finie, on libère le verrou et on change manuellement le nombre de places dans la file d'attente. */
    workerQueues[minQueueIndex]--;
    pthread_mutex_unlock(&queueMutex);

    /* 7. Envoyer la tache à la carte la moins utilisée. */
    pthread_mutex_lock(&idMutex);
    header.taskID = currentTaskID;
    routingTable[currentTaskID] = arg->socket;
    currentTaskID = (currentTaskID + 1) % MAX_CONCURRENT_TASKS;
    pthread_mutex_unlock(&idMutex);

    pthread_mutex_lock(&workerMutexes[minQueueIndex]);

    if (header.action == 0) {
        /* Si on souhaite utiliser un modèle. */
        /* Envoi du header. */
        if (sendMessage(workerSocketTable[minQueueIndex], (const char *) &header, sizeof(header)) == -1) {
            printf("ERREUR : L'envoi du header à la orin-nano-%d à échoué.\n", minQueueIndex);
            goto cleanup;
        }
        printf("Envoi du header réussi à la orin-nano-%d.\n", minQueueIndex);

        /* Envoi du fichier. */
        if (sendMessage(workerSocketTable[minQueueIndex], fileString1, header.messageSize) == -1) {
            printf("ERREUR : L'envoi de la tâche à la orin-nano-%d à échoué.\n", minQueueIndex);
            goto cleanup;
        }
        printf("Envoi de la tâche réussie à la orin-nano-%d.\n", minQueueIndex);
    } else {
        /* Si on souhaite soumettre un modèle. */
        header.messageSize = fileSize1;

        /* Envoi du premier header. */
        if (sendMessage(workerSocketTable[minQueueIndex], (const char *) &header, sizeof(header)) == -1) {
            printf("ERREUR : L'envoi du premier header à la orin-nano-%d à échoué.\n", minQueueIndex);
            goto cleanup;
        }
        printf("Envoi du premier header réussi à la orin-nano-%d.\n", minQueueIndex);

        /* Envoi du fichier .onnx. */
        if (sendMessage(workerSocketTable[minQueueIndex], fileString1, fileSize1) == -1) {
            printf("ERREUR : L'envoi du fichier .onnx à la orin-nano-%d à échoué.\n", minQueueIndex);
            goto cleanup;
        }

        /* Envoi du second header. */
        header.messageSize = fileSize2;
        if (sendMessage(workerSocketTable[minQueueIndex], (const char *) &header, sizeof(header)) == -1) {
            printf("ERREUR : L'envoi du second header à la orin-nano-%d à échoué.\n", minQueueIndex);
            goto cleanup;
        }
        printf("Envoi du second header réussi à la orin-nano-%d.\n", minQueueIndex);

        /* Envoi du fichier .cpp. */
        if (sendMessage(workerSocketTable[minQueueIndex], fileString2, fileSize2) == -1) {
            printf("ERREUR : L'envoi du fichier .cpp à la orin-nano-%d à échoué.\n", minQueueIndex);
            goto cleanup;
        }
        printf("Envoi de la tâche réussie à la orin-nano-%d.\n", minQueueIndex);
    }

    cleanup:
    pthread_mutex_unlock(&workerMutexes[minQueueIndex]);
    return NULL;
}


/** Gère le lancement des threads pour les connexions clients.
 * @param clientSocket Le socket sur lequel le client communique
 * @param index L'index du client
 */
void handleConnection(socket_t clientSocket, int index) {
    args[index].socket = clientSocket;
    args[index].index = index;
    args[index].priority = -1;
    if (pthread_create(&clientHandlerThreads[index], NULL, clientListening, &args[index]) != 0) {
        printf("ERREUR : Le thread d'écoute client n'a pas pu être lancé.\n");
    } else {
        printf("Création du thread d'écoute client numéro %d.\n", index);
        pthread_detach(clientHandlerThreads[index]);
        socketTable[index] = 0;
    }
}


/** Permet de recevoir les messages de monitoring de la
 * part des workers.
 * @param workerSocket Le socket depuis lequel récupérer les informations.
 * @param workerIndex L'index de la carte
 */
void getInformationFromWorker(socket_t workerSocket, int workerIndex) {
    (void) workerIndex;

    /* Recevoir le header d'information. */
    struct monitoringMessage message;
    memset(&message, 0, sizeof(message));

    if (receiveMessage(workerSocket, &message, sizeof(message)) == -1) {
        printf("ERREUR : Le message de monitoring d'une des cartes n'a pas pu être reçu.\n");
        CLOSE_SOCKET(workerSocket);
        monitoringSocketTable[workerIndex] = 0;
        return;
    }
    printf("Header d'information reçu pour la orin-nano-%d, type : %s.\n", message.workerIndex, message.type);

    if (strcmp(message.type, "INFO") == 0) {
        /* Si son type est INFO, mettre à jour l'entrée correspondante de workerQueues. */
        pthread_mutex_lock(&queueMutex);
        workerQueues[message.workerIndex] = message.sizeLeft;
        pthread_mutex_unlock(&queueMutex);
        
        /* Affichage informatif. */
        printf("\nEtat des files d'attentes :\n");
        for (int i = 0; i < NUM_CARDS; i++) {
            if (connectedCards[i] != 0) {
                printf("File d'attente orin-nano-%d : %d/%d\n", i, MAX_QUEUE_SIZE - workerQueues[i], MAX_QUEUE_SIZE);
            }
        }
        printf("\n");
    } else if (strcmp(message.type, "HELLO") == 0) {
        /* Créer et stocker le socket. */
        int port = 5798;
        socket_t connectionTestingSocket = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in jetsonAddress;

        memset(&jetsonAddress, 0, sizeof(jetsonAddress));

        /* Instantiation des paramètres du socket. */
        char ipAddress[16];
        memset(ipAddress, 0, sizeof(ipAddress));
        sprintf(ipAddress, "147.127.121.%d", message.workerIndex + destinationOffset);

        jetsonAddress.sin_addr.s_addr = inet_addr(ipAddress);
        jetsonAddress.sin_family = AF_INET;
        jetsonAddress.sin_port = htons(port);

        if (connect(connectionTestingSocket, (struct sockaddr *) &jetsonAddress, sizeof(jetsonAddress)) == -1) {
            printf("ERREUR : La connexion à la carte orin-nano-%d est impossible.\n", message.workerIndex);
            return;
        }
        workerSocketTable[message.workerIndex] = connectionTestingSocket;

        workerArgs[message.workerIndex].socket = workerSocketTable[message.workerIndex];
        workerArgs[message.workerIndex].index = message.workerIndex;
        workerArgs[message.workerIndex].priority = -1;

        if (pthread_create(&jetsonListenerThreads[message.workerIndex], NULL, workerListening, &workerArgs[message.workerIndex]) != 0) {
            printf("ERREUR : Le thread d'écoute worker n'a pas pu être lancé.\n");
        } else {
            pthread_detach(jetsonListenerThreads[message.workerIndex]);
            printf("Lancement du thread d'écoute pour la orin-nano-%d.\n", workerArgs[message.workerIndex].index);
        }

        /* Si son type est HELLO, déclarer connectedCards[i] = 1. */
        printf("Connexion de la carte orin-nano-%d\n", message.workerIndex);
        connectedCards[message.workerIndex] = 1;
    } else if (strcmp(message.type, "BYE") == 0) {
        /* Si son type est BYE, déclarer connectedCards[i] = 0. */
        printf("Déconnexion de la carte orin-nano-%d\n", message.workerIndex);

        pthread_mutex_lock(&queueMutex);
        workerQueues[message.workerIndex] = 0;
        pthread_mutex_unlock(&queueMutex);


        CLOSE_SOCKET(workerSocket);
        monitoringSocketTable[workerIndex] = 0;
        connectedCards[message.workerIndex] = 0;
        CLOSE_SOCKET(workerSocketTable[message.workerIndex]);
        workerSocketTable[message.workerIndex] = 0;
    }
}


/** Thread d'écoute de l'état des workers, qui appelle
 * getInformationFromWorker lorsque l'un des sockets change
 * d'état.
*/
void* listenToWorkers(void *) {
    for (int i = 0; i < NUM_CARDS; i++) {
        workerQueues[i] = 0;
    }

    struct fd_set workersReadSet;

    for (int i = 0; i < NUM_CARDS; i++) {
        monitoringSocketTable[i] = 0;
    }

    socket_t monitoringSocket;
    if ((monitoringSocket = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        printf("ERREUR : Le socket n'a pas pu être créé.\n");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in orchestratorAddress;
    memset(&orchestratorAddress, 0, sizeof(orchestratorAddress));
    orchestratorAddress.sin_addr.s_addr = ADDR_ANY;
    orchestratorAddress.sin_family = AF_INET;
    orchestratorAddress.sin_port = htons(9988);

    if ((bind(monitoringSocket, (const struct sockaddr *) &orchestratorAddress, sizeof(orchestratorAddress))) == SOCKET_ERROR) {
        printf("ERREUR : Le bind au port %d n'a pas pu être réalisé.\n", PORT);
        exit(EXIT_FAILURE);
    }

    if (listen(monitoringSocket, MAX_NUM_CONNECTION) == -1) {
        printf("ERREUR : Le listen pour le socket à échoué.\n");
        exit(EXIT_FAILURE);
    }

    while (isRunning == 1) {
        checkForConnections(&workersReadSet, &monitoringSocket, monitoringSocketTable, NUM_CARDS, 1, getInformationFromWorker);
    }
    return NULL;
}


/** Fonction helper pour écouter sur un set de socket
 * et appeler la fonction handler lorsque un des socket
 * voit son état changer.
 * @param fdSet Le set à surveiller
 * @param mainSocket Le socket principal inséré en premier
 * @param socketTable La table contenant les différents sockets à écouter
 * @param tableSize La table de la socketTable
 * @param read Vaut 1 si le set est un read set et 0 sinon
 * @param handler Fonction à appeler lorsqu'un socket change d'état
 */
void checkForConnections(fd_set *fdSet, socket_t *mainSocket, socket_t *socketTable, int tableSize, int read, void handler(socket_t, int)) {
    SOCKET clientSocket;
    struct sockaddr_in clientAddress;
    int addressLength = sizeof(struct sockaddr_in);
    
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    /* Réinitialisation des connections surveillées. */
    FD_ZERO(fdSet);
    FD_SET(*mainSocket, fdSet);

    /* Insertion des sockets clients connus dans le set du select. */
    int highestSocketFd = *mainSocket;
    for (int i = 0; i < tableSize; i++) {
        if (socketTable[i] == 0) {
            continue;
        }

        /* Utile pour la compatibilité sur systèmes POSIX. */
        if ((int) socketTable[i] > highestSocketFd) {
            highestSocketFd = socketTable[i];
        }

        FD_SET(socketTable[i], fdSet);
    }
    
    /* 3. Accepter les connexions entrantes et stocker les sockets. */
    int selectReturn = -1;
    if (read == 1) {
        selectReturn = select(highestSocketFd + 1, fdSet, NULL, NULL, &timeout);
    } else {
        selectReturn = select(highestSocketFd + 1, NULL, fdSet, NULL, &timeout);
    }

    if (selectReturn < 0) {
        return;
    }

    /* Si le socket du serveur est prêt (il reçoit une requête), on l'accepte. */
    if (FD_ISSET(*mainSocket, fdSet)) {
        if ((clientSocket = (accept(*mainSocket, (struct sockaddr *) &clientAddress, &addressLength))) == INVALID_SOCKET) {
            if (isRunning == 0) return;
            printf("ERREUR : La connexion au client s'est mal passée.\n");
            return;
        }
        printf("Une nouvelle connexion s'est produite, insertion dans la table de socket.\n");

        /* On insère le socket reçu dans le premier espace libre du tableau. */
        for (int i = 0; i < tableSize; i++) {
            if (socketTable[i] == 0) {
                socketTable[i] = clientSocket;
                break;
            }
        }
    }

    /* On vérifie l'état des sockets, si l'un d'entre eux à une requête, on la traite. */
    for (int i = 0; i < tableSize; i++) {
        if (socketTable[i] == 0) {
            continue;
        }
        if (FD_ISSET(socketTable[i], fdSet)) {
            /* 4. Traiter les demandes lorsqu'elles arrivent (réception du header puis du fichier .pgm). */
            printf("Traitement de la connexion.\n");
            handler(socketTable[i], i);
        }
    }
}


/** Thread propre à chaque connexion à une carte, permettant
 *  d'écouter pour réceptionner les résultats depuis cette dernière.
 * @param _arg Les paramètres utiles au thread
 */
void* workerListening(void* _arg) {
    pthread_args *arg = (pthread_args *) _arg;

    while (isRunning == 1) {
        fd_set socketReadSet;

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int selectReturn = 0;
        while (selectReturn <= 0) {
            FD_ZERO(&socketReadSet);
            FD_SET(arg->socket, &socketReadSet);
            selectReturn = select(arg->socket + 1, &socketReadSet, NULL, NULL, &timeout);
            if (selectReturn == -1) break;
        }

        /* 8. Réceptionner les résultats. */
        struct messageHeader header;
        if (receiveMessage(arg->socket, &header, sizeof(header)) == -1) {
            if (connectedCards[arg->index] == 0 || isRunning == 0) break;
            printf("ERREUR : Le header n'a pas pu être reçu depuis la orin-nano-%d\n", arg->index);
            break;
        }
        printf("Réception d'un header depuis la orin-nano-%d.\n", arg->index);

        if (header.action == 1) {
            /* La soumission d'un nouveau modèle à fonctionnée. */
            sendMessage(routingTable[header.taskID], (const char *) &header, sizeof(header));
            printf("Le modèle %s à bien été enregistré par la orin-nano-%d\n", header.model, arg->index);
            /* On ajoute le nouveau modèle à validModelNames. */
            validModelNames[numModels++] = strdup(header.model);
            goto end_task;
        } else if (header.action == 2) {
            /* Une erreur de compilation ou d'exécution s'est produite au niveau de la worker. */
            sendMessage(routingTable[header.taskID], (const char *) &header, sizeof(header));
            printf("ERREUR : La worker n'a pas pu compiler/exécuter la tâche.\n");
            goto end_task;
        }

        char *resultString = malloc(header.messageSize);
        if (receiveMessage(arg->socket, resultString, header.messageSize) == -1) {
            printf("ERREUR : La réception du résultat depuis orin-nano-%d à échoué.\n", arg->index);
            free(resultString);
            goto end_task;
        }
        printf("Réception du résultat depuis la orin-nano-%d.\n", arg->index);

        /* 9. Envoyer le résultat au client concerné. */
        if (sendMessage(routingTable[header.taskID], (const char *) &header, sizeof(header)) == -1) {
            printf("ERREUR : Le header n'a pas pu être envoyé au client.\n");
            free(resultString);
            goto end_task;
        }
        printf("Envoi du header au client depuis la orin-nano-%d.\n", arg->index);

        if (sendMessage(routingTable[header.taskID], resultString, header.messageSize) == -1) {
            printf("ERREUR : Le résultat n'a pas pu être envoyé au client.\n");
        }
        printf("Envoi du résultat au client depuis la orin-nano-%d.\n", arg->index);

        end_task:
        CLOSE_SOCKET(routingTable[header.taskID]);
        routingTable[header.taskID] = 0;
        printf("Réception terminée, coupure de la connexion au client depuis la orin-nano-%d\n", arg->index);
    }
    return NULL;
}


/** Fonction principale. */
int main(void) {
    initInteruptHandling();
    printf("Gestion des interruptions initialisée.\n");

    /* Initialisation de la connexion windows. */
    init_connection();
    printf("Connexion initialisée.\n");

    /* Récupération des noms de modèles existants. */
    int savedModels = open("currentModels.txt", O_RDWR);

    if (savedModels == -1) {
        printf("ERREUR : Impossible d'ouvrir le fichier de sauvegarde des modèles.\n");
        exit(EXIT_FAILURE);
    }

    char readCharacter;
    int readBytes;
    char modelName[64];
    int modelNameIndex = 0;
    while ((readBytes = read(savedModels, &readCharacter, 1)) > 0) {
        if (readCharacter == '\n') {
            modelName[modelNameIndex] = '\0';
            validModelNames[numModels++] = strdup(modelName);
            modelNameIndex = 0;
            memset(modelName, 0, sizeof(modelName));
        } else {
            if (modelNameIndex >= 63) {
                printf("ERREUR : Le nom de modèle fourni est trop long.\n");
                exit(EXIT_FAILURE);
            }
            modelName[modelNameIndex++] = readCharacter;
        }
    }
    close(savedModels);

    /* Initialisation de la taille restante des queues des workers. */
    for (int i = 0; i < NUM_CARDS; i++) {
        workerQueues[i] = -1;
    }
    printf("File d'attente des workers initialisée.\n");

    /* Initilisation des verrous de worker et du verrou global de consultation des files d'attente. */
    pthread_mutex_init(&queueMutex, NULL);
    pthread_mutex_init(&idMutex, NULL);
    for (int i = 0; i < NUM_CARDS; i++) {
        pthread_mutex_init(&workerMutexes[i], NULL);
    }
    printf("Mutexes des queues, id de tâche et des cartes initialisés.\n");

    /* 1. Créer un socket pour accepter les connexions client entrantes. */
    socket_t orchestratorSocket;
    initializeSocket(&orchestratorSocket);
    printf("Socket de l'orchestrateur initialisé.\n");

    /* 2. Garder en mémoire tous les sockets client et worker actuellement utilisés. */
    struct fd_set socketReadSet;

    for (int i = 0; i < MAX_NUM_CONNECTION; i++) {
        socketTable[i] = 0;
    }

    for (int i = 0; i < NUM_CARDS; i++) {
        workerSocketTable[i] = 0;
    }
    printf("Tables de sockets client et worker initialisées.\n");

    memset(routingTable, 0, sizeof(routingTable));
    memset(jetsonListenerThreads, 0, sizeof(jetsonListenerThreads));
    memset(clientHandlerThreads, 0, sizeof(clientHandlerThreads));

    /* Lancement du thread d'écoute de l'état des workers. */
    pthread_t workerStateThread;
    if ((pthread_create(&workerStateThread, NULL, listenToWorkers, NULL)) != 0) {
        printf("ERREUR : La création du thread d'écoute de l'état des cartes à échoué.\n");
        exit(EXIT_FAILURE);
    }
    printf("Lancement du thread d'écoute de l'état des workers.\n");

    /* Boucle de fonctionnement de l'orchestrateur. */
    while (isRunning == 1) {
        checkForConnections(&socketReadSet, &orchestratorSocket, socketTable, MAX_NUM_CONNECTION, 1, handleConnection);
    }

    /* Envoi en broadcast du message de fermeture de l'orchestrateur. */
    struct messageHeader header;
    memset(&header, 0, sizeof(header));
    header.action = 5;
    for (int i = 0; i < NUM_CARDS; i++) {
        if (connectedCards[i] != 1) continue;
        if (sendMessage(workerSocketTable[i], (const char *) &header, sizeof(header)) == -1) {
            printf("ERREUR : Le message de fermeture de l'orchestrateur n'a pas pu être envoyé à la carte orin-nano-%d\n", i);
        }
        CLOSE_SOCKET(workerSocketTable[i]);
    }

    /* Sauvegarde des modèles existants dans un fichier .txt. */
    FILE *saveFile = fopen("currentModels.txt", "w");
    if (saveFile == NULL) {
        printf("ERREUR : Impossible de créer le fichier de sauvegarde.\n");
        return -1;
    }

    for (int i = 0; i < numModels; i++) {
        fprintf(saveFile, "%s\n", validModelNames[i]);
    }
    fclose(saveFile);
    
    /* Fermeture de la connexion (windows) et libération de la mémoire allouée. */
    close_connection();
    printf("Fermeture de l'orchestrateur.\n");
    return 0;
}
