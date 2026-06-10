#include "orchestrator.h"


/* Nombre de cartes connectées sur le réseau. */
int numConnectedCards = 0;
/* Index de la dernière carte connectée. */
int lastCardIndex = 0;

/* Premier offset d'adresses IP. */
int destinationOffset = 94;

/* Tableau contenant les places en file d'attente de chaque worker. */
int workerQueues[NUM_CARDS];


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


/** Point d'entrée pour chaque thread afin de créer les sockets et
 * gérer l'envoi et la reception des informations utiles.
 * @param _args Un structure contenant les informations utiles à l'échange
 */
void* threadMain(void* _arg) {
    /* Récuperation de la structure contenant des informations. */
    pthread_args *arg = (pthread_args*) _arg;
    /* 3. Créer un socket pour se connecter à une jetson nano. */
    int port = 5798;
    int destinationOffset = 94;
    socket_t clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serverAddress;

    memset(&serverAddress, 0, sizeof(serverAddress));

    /* Instantiation des paramètres du socket. */
    arg->ipAddress = malloc(16 * sizeof(char));
    sprintf(arg->ipAddress, "147.127.121.%d", arg->index + destinationOffset);
    serverAddress.sin_addr.s_addr = inet_addr(arg->ipAddress);
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);

    if ((connect(clientSocket, (struct sockaddr *) &serverAddress, sizeof(serverAddress))) == -1) {
        printf("ERREUR : Connexion échouée à la jetson nano orin-nano-%d\n", arg->index);
        goto end_connection;
    }
    printf("Connexion réussie à la jetson nano orin-nano-%d\n", arg->index);

    /* 4. Envoyer l'en-tête, les paramètres et le fichier .c. */
    
    /* Instantiation du header. */
    struct messageHeader header;
    header.priority = arg->priority;

    /* On envoie le header réseau. */
    if ((sendMessage(clientSocket, (const char *) &header, sizeof(header))) == -1) {
        printf("ERREUR : L'envoi du header à orin-nano-%d s'est mal déroulé\n", arg->index);
        goto end_connection;
    }
    
    /* 5. Attendre la réception des résultats pour reconstruire la matrice C. */
    int receivedBytes = 0;
    int bytesToReceive = 0 * sizeof(int);
    int bytesLeft = bytesToReceive;
    arg->result = malloc(bytesToReceive); 
    while (receivedBytes < bytesToReceive) {
        int bytesReceived = (recv(clientSocket, (char*) arg->result + receivedBytes, bytesLeft, 0));
        if (bytesReceived <= 0) {
            printf("ERREUR : Echec lors de la réception du résultat depuis orin-nano-%d\n", arg->index);
            goto end_connection;
        }
        receivedBytes += bytesReceived;
        bytesLeft -= bytesReceived;
    }
    printf("Confirmation de la réception de la matrice C de orin-nano-%d\n", arg->index);
    end_connection:
    CLOSE_SOCKET(clientSocket);
    return NULL;
}


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
 * incrémente connectedCards en conséquence.
 * @param connectedCards Le nombre de cartes connectées et découvertes
 */
void checkConnectedJetsons(int* connectedCards) {
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
        CLOSE_SOCKET(jetsonSockets[i]);
    }
    printf("==========================================================\n\n");
}


/** Initialise les threads avec des informations comme l'adresse à laquelle se connecter,
 * l'index du thread et la priorité de la tâche.
 */
void initializeAndStartThreads(pthread_args *args, int *connectedCards, pthread_t *threads) {
    /* Initialisation des arguments des threads. */
    for (int n = 0; n < NUM_CARDS; n++) {
        args[n].index = n;
        args[n].result = NULL;
        args[n].priority = 0;
        args[n].ipAddress = malloc(16 * sizeof(char));
        sprintf(args[n].ipAddress, "147.127.121.%d", n + destinationOffset);
    }

    /* Récupération du nombre effectif de cartes connectées et assignation de l'indice logique. */
    for (int i = 0; i < NUM_CARDS; i++) {
        if (connectedCards[i] == 1) {
            args[i].connectedIndex = numConnectedCards;
            numConnectedCards++;
            lastCardIndex = i;
        }
    }

    /* Création des threads.*/
    for (int n = 0; n < NUM_CARDS; n++) {
        if (connectedCards[n] == 0) {
            continue;
        }
        if ((pthread_create(&threads[n], NULL, threadMain, &args[n])) != 0) {
            printf("ERREUR : Le thread n'a pas pu être créé.\n");
        }
    }
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


void handleConnection(socket_t *clientSocket) {
    /* 5. Envoyer la tache à la carte la moins utilisée. */
    /* 6. Réceptionner les résultats. */
    /* 7. Envoyer le résultat au client concerné. */
}


void getInformationFromWorker(socket_t *socket) {
    
}


void* listenToWorkers(void *) {
    for (int i = 0; i < NUM_CARDS; i++) {
        workerQueues[i] = 0;
    }

    struct fd_set workersReadSet;
    socket_t monitoringSocketTable[NUM_CARDS];

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

    while (1) {
        checkForConnections(&workersReadSet, &monitoringSocket, monitoringSocketTable, sizeof(monitoringSocketTable), 1, getInformationFromWorker);
    }
}


void checkForConnections(fd_set *fdSet, socket_t *mainSocket, int *socketTable, int tableSize, int read, void handler(socket_t *)) {
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
    int highestSocketFd = 0;
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
    if (read == 1) {
        select(highestSocketFd, fdSet, NULL, NULL, &timeout);
    } else {
        select(highestSocketFd, NULL, fdSet, NULL, &timeout);
    }

    /* Si le socket du serveur est prêt (il reçoit une requête), on l'accepte. */
    if (FD_ISSET(*mainSocket, fdSet)) {
        if ((clientSocket = (accept(*mainSocket, (struct sockaddr *) &clientAddress, &addressLength))) == INVALID_SOCKET) {
            printf("ERREUR : La connexion au client s'est mal passée.\n");
            return;
        }

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
            handler(&socketTable[i]);
            socketTable[i] = 0;
        }
    }
}


/** Fonction principale permettant de :
 * - Vérifier les inputs,
 * - Créer les matrices,
 * - Créer les sockets,
 * - Envoyer l'information utile,
 * - Réceptionner les résultats des cartes jetson,
 * - Reconstruire l'information finale,
 * - Vérifier la conformité de l'information et mesurer le temps total pour un calcul par CPU et par GPU distribué.
*/
int main(void) {
    /* Initialisation de la connexion windows. */
    init_connection();

    /* 1. Créer un socket pour accepter les connexions entrantes. */
    socket_t orchestratorSocket;
    initializeSocket(&orchestratorSocket);

    /* 2. Garder en mémoire tous les sockets actuellement utilisés. */
    struct fd_set socketReadSet;

    socket_t socketTable[MAX_NUM_CONNECTION];
    for (int i = 0; i < MAX_NUM_CONNECTION; i++) {
        socketTable[i] = 0;
    }

    /* Récupèration du nombre de cartes connectées et leurs index. */
    int connectedCards[NUM_CARDS];
    checkConnectedJetsons(connectedCards);

    /* Thread d'écoute de l'état des workers. */
    pthread_t workerStateThread;
    if ((pthread_create(&workerStateThread, NULL, listenToWorkers, NULL)) != 0) {
        printf("ERREUR : La création du thread d'écoute de l'état des cartes à échoué.\n");
        exit(EXIT_FAILURE);
    }

    /* Boucle de fonctionnement de l'orchestrateur. */
    while (1) {
        checkForConnections(&socketReadSet, &orchestratorSocket, socketTable, sizeof(socketTable), 1, handleConnection);
        
        /* 2. Créer NUM_CARDS thread pour gérer les sockets et le résultat de chaque worker. */
        pthread_t *threads = malloc(NUM_CARDS * sizeof(pthread_t));
        pthread_args *args = malloc(NUM_CARDS * sizeof(pthread_args));

        /* Initialisation et création des threads. */
        initializeAndStartThreads(args, connectedCards, threads);

        /* Attente de la terminaison des threads. */
        for (int n = 0; n < NUM_CARDS; n++) {
            pthread_join(threads[n], NULL);
        }
        
        /* Fermeture de la connexion (windows) et libération de la mémoire allouée. */
        close_connection();
        for (int n = 0; n < NUM_CARDS; n++) {
            free(args[n].ipAddress);
            if (args[n].result) {
                free(args[n].result);
            }
        }
        free(threads);
        free(args);
    }

    WSACleanup();
    return 0;
}
