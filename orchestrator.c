#include "orchestrator.h"


/* Nombre de cartes connectées sur le réseau. */
int numConnectedCards = 0;
/* Index de la dernière carte connectée. */
int lastCardIndex = 0;

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


/** Détecte sur le réseau la présence de cartes jetsons et
 * incrémente connectedCards en conséquence. Conserve également
 * dans la workerSocketTable les sockets vers les workers.
 */
void checkConnectedJetsons() {
    printf("\n\n\n==========================================================\n");
    printf("Détection des cartes présentes sur le réseau...\n");

    struct fd_set socketWriteSet;
    FD_ZERO(&socketWriteSet);

    SOCKET jetsonSockets[NUM_CARDS];
    int maxSocketFd = 0;

    for (int i = 0; i < NUM_CARDS; i++) {
        connectedCards[i] = 0;

        /* Créer un socket pour tester la connexion à une jetson nano. */
        int port = 5798;
        socket_t connectionTestingSocket = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in jetsonAddress;

        memset(&jetsonAddress, 0, sizeof(jetsonAddress));

        /* Instantiation des paramètres du socket. */
        char ipAddress[16];
        memset(ipAddress, 0, sizeof(ipAddress));
        sprintf(ipAddress, "147.127.121.%d", i + destinationOffset);
        jetsonAddress.sin_addr.s_addr = inet_addr(ipAddress);
        jetsonAddress.sin_family = AF_INET;
        jetsonAddress.sin_port = htons(port);
        
        /* On rend le socket non-bloquant pour pouvoir essayer la connexion sur plusieurs cartes à la fois. */
        setNonBlocking(connectionTestingSocket);
        connect(connectionTestingSocket, (struct sockaddr *) &jetsonAddress, sizeof(jetsonAddress));
        jetsonSockets[i] = connectionTestingSocket;

        /* Récupération du descripteur de socket maximum, pour le select sur systèmes UNIX. */
        if ((int) connectionTestingSocket > maxSocketFd) maxSocketFd = jetsonSockets[i];
    }
    
    /* Vérification de l'état du lien avec les cartes. */
    for (int i = 0; i < 10; i++) {
        TIMEVAL timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 50000;

        FD_ZERO(&socketWriteSet);
        
        /* A chaque select, on insère dans le set les cartes n'ayant pas été détectées. */
        for (int j = 0; j < NUM_CARDS; j++) {
            if (connectedCards[j] == 0) {
                FD_SET(jetsonSockets[j], &socketWriteSet);
            }
        }

        /* Si au moins un descripteur à eu un changement d'état.*/
        if (select(maxSocketFd + 1, NULL, &socketWriteSet, NULL, &timeout) > 0) {
            for (int k = 0; k < NUM_CARDS; k++) {
                if (FD_ISSET(jetsonSockets[k], &socketWriteSet) != 0) {
                    connectedCards[k] = 1;
                    workerSocketTable[k] = jetsonSockets[k];
                    FD_CLR(jetsonSockets[k], &socketWriteSet);
                }
            }
        }
    }

    /* Pour l'affichage et la fermeture des sockets ouverts. */
    for (int i = 0; i < NUM_CARDS; i++) {
        if (connectedCards[i] == 0) {
            printf("La orin-nano-%d n'est pas présente sur le réseau.\n", i);
        } else {
            printf("La orin-nano-%d est présente sur le réseau.\n", i);
        }
    }
    printf("==========================================================\n\n");
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
    }
    selectReturn = 0;

    /* Récupérer le header depuis le client. */
    struct messageHeader header;
    if (receiveMessage(arg->socket, &header, sizeof(header)) == -1) {
        printf("ERREUR : La réception du header du client %d s'est mal passée.\n", arg->index);
        return NULL;
    }
    printf("Réception du header du client %d.\n", arg->index);

    /* 5. Récupérer le fichier .cu depuis le clientSocket. */
    char *fileString = malloc(header.messageSize * sizeof(char));
    if (receiveMessage(arg->socket, fileString, header.messageSize * sizeof(char)) == -1) {
        printf("ERREUR : La réception du fichier .cu n'a pas aboutie.\n");
        return NULL;
    }
    printf("Réception du fichier .cu du client %d.\n", arg->index);

    /* 6. Trouver la carte la moins utilisée par exploration de workerQueues. */
    int tries = 1;
    try_queues:
    pthread_mutex_lock(&queueMutex);
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
            return NULL;
        }

        tries++;
        goto try_queues;
    }
    printf("La carte la moins remplie est la orin-nano-%d.\n", minQueueIndex);

    /* L'exploration est finie, on libère le verrou et on change manuellement le nombre de places dans la file d'attente. */
    workerQueues[minQueueIndex]++;
    pthread_mutex_unlock(&queueMutex);

    /* 7. Envoyer la tache à la carte la moins utilisée. */
    pthread_mutex_lock(&idMutex);
    header.taskID = currentTaskID;
    routingTable[currentTaskID] = arg->socket;
    currentTaskID = (currentTaskID + 1) % MAX_CONCURRENT_TASKS;
    pthread_mutex_unlock(&idMutex);

    pthread_mutex_lock(&workerMutexes[minQueueIndex]);
    if (sendMessage(workerSocketTable[minQueueIndex], (const char *) &header, sizeof(header)) == -1) {
        printf("ERREUR : L'envoi du header à la orin-nano-%d à échoué.\n", minQueueIndex);
        return NULL;
    }
    printf("Envoi du header réussi à la orin-nano-%d.\n", minQueueIndex);

    if (sendMessage(workerSocketTable[minQueueIndex], fileString, header.messageSize * sizeof(char)) == -1) {
        printf("ERREUR : L'envoi de la tâche à la orin-nano-%d à échoué.\n", minQueueIndex);
        return NULL;
    }
    printf("Envoi de la tâche réussie à la orin-nano-%d.\n", minQueueIndex);
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
    }
}


/** Permet de recevoir les messages de monitoring de la
 * part des workers.
 * @param workerSocket Le socket depuis lequel récupérer les informations.
 * @param workerIndex L'index de la carte
 */
void getInformationFromWorker(socket_t workerSocket, int workerIndex) {
    (void) workerIndex;
    printf("Un message à été reçu ! J'essaie de le recevoir.\n");

    /* Recevoir le header d'information. */
    struct monitoringMessage message;
    memset(&message, 0, sizeof(message));

    if (receiveMessage(workerSocket, &message, sizeof(message)) == -1) {
        printf("ERREUR : Le message de monitoring d'une des cartes n'a pas pu être reçu.\n");
        return;
    }
    printf("Header d'information reçu pour la orin-nano-%d, type : %s.\n", message.workerIndex, message.type);

    if (strcmp(message.type, "INFO") == 0) {
        /* Si son type est INFO, mettre à jour l'entrée correspondante de workerQueues. */
        workerQueues[message.workerIndex] = message.sizeLeft;
        
        /* Affichage informatif. */
        printf("\nEtat des files d'attentes :\n");
        for (int i = 0; i < NUM_CARDS; i++) {
            printf("File d'attente orin-nano-%d : %d\n", i, workerQueues[i]);
        }
        printf("\n");
    } else if (strcmp(message.type, "HELLO") == 0) {
        /* Si son type est HELLO, déclarer connectedCards[i] = 1. */
        printf("Connexion de la carte orin-nano-%d\n", message.workerIndex);
        connectedCards[message.workerIndex] = 1;
    } else if (strcmp(message.type, "BYE") == 0) {
        /* Si son type est BYE, déclarer connectedCards[i] = 0. */
        printf("Déconnexion de la carte orin-nano-%d\n", message.workerIndex);
        connectedCards[message.workerIndex] = 0;
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
    socket_t monitoringSocketTable[NUM_CARDS];

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
    } else if (selectReturn > 0) { 
        printf("%d sockets ont changé d'état.\n", selectReturn);
    }

    /* Si le socket du serveur est prêt (il reçoit une requête), on l'accepte. */
    if (FD_ISSET(*mainSocket, fdSet)) {
        if ((clientSocket = (accept(*mainSocket, (struct sockaddr *) &clientAddress, &addressLength))) == INVALID_SOCKET) {
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
            /* 4. Traiter les demandes lorsqu'elles arrivent (réception du header puis du fichier .cu). */
            printf("Traitement de la connexion.\n");
            handler(socketTable[i], i);
        }
    }
}


/** Thread propre à chaque connexion à un carte,
 * permettant d'écouter pour réceptionner les résultats depuis
 * cette dernière.
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
        }
        selectReturn = 0;

        /* 8. Réceptionner les résultats. */
        struct messageHeader header;
        if (receiveMessage(arg->socket, &header, sizeof(header)) == -1) {
            printf("ERREUR : Le header n'a pas pu être reçu depuis la orin-nano-%d\n", arg->index);
            goto end;
        }
        printf("Réception d'un header depuis la orin-nano-%d.\n", arg->index);

        char *resultString = malloc(header.messageSize * sizeof(char));
        if (receiveMessage(arg->socket, resultString, header.messageSize * sizeof(char)) == -1) {
            printf("ERREUR : La réception du résultat depuis orin-nano-%d à échoué.\n", arg->index);
            goto end_task;
        }
        printf("Réception du résultat depuis la orin-nano-%d.\n", arg->index);

        /* 9. Envoyer le résultat au client concerné. */
        if (sendMessage(routingTable[header.taskID], (const char *) &header, sizeof(header)) == -1) {
            printf("ERREUR : Le header n'a pas pu être envoyé au client.\n");
            goto end_task;
        }
        printf("Envoi du header au client depuis la orin-nano-%d.\n", arg->index);

        if (sendMessage(routingTable[header.taskID], resultString, header.messageSize * sizeof(char)) == -1) {
            printf("ERREUR : Le résultat n'a pas pu être envoyé au client.\n");
        }
        printf("Envoi du résultat au client depuis la orin-nano-%d.\n", arg->index);

        end_task:
        CLOSE_SOCKET(routingTable[header.taskID]);
        routingTable[header.taskID] = 0;
        free(resultString);
        end:
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

    /* Récuperation du nombre de cartes connectées et de leurs index. */
    checkConnectedJetsons();

    /* Instantiation des threads d'écoute pour les workers. */
    for (int i = 0; i < NUM_CARDS; i++) {
        if (connectedCards[i] == 0) {
            continue;
        }
        workerArgs[i].socket = workerSocketTable[i];
        workerArgs[i].index = i;
        workerArgs[i].priority = -1;
        if (pthread_create(&jetsonListenerThreads[i], NULL, workerListening, &workerArgs[i]) != 0) {
            printf("ERREUR : Le thread d'écoute worker n'a pas pu être lancé.\n");
        } else {
            pthread_detach(jetsonListenerThreads[i]);
            printf("Lancement du thread d'écoute pour la orin-nano-%d.\n", workerArgs[i].index);
        }
    }

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
    
    /* Fermeture de la connexion (windows) et libération de la mémoire allouée. */
    close_connection();
    printf("Fermeture de l'orchestrateur.\n");
    return 0;
}
