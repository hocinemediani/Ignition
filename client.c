/* Objectif, distribuer le calcul matriciel C = A x B entre les jetsons. */
#include "client.h"

/* Instructions préprocesseur pour différencier les architectures linux de windows qui ont des imports différents. */
#ifdef _WIN32
    #include <winsock2.h>
    #include <process.h>
    #include <io.h>

    typedef SOCKET socket_t;
    #define CLOSE_SOCKET closesocket

    void init_connection() {
        WSADATA wsadata;
        WSAStartup(MAKEWORD(2, 2), &wsadata);
    }

    void close_connection() {
        WSACleanup();
    }
#else
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/ioctl.h>

    typedef int socket_t;
    #define CLOSE_SOCKET close

    void init_connection() {};
    void close_connection() {};
#endif

/* Structure pour envoyer et récupérer les informations par le socket. */
typedef struct pthread_args {
    int index;
    int connectedIndex;
    int* result;
    int numRows;
    int matrixOffset;
    char* ipAddress;
    int priority;
} pthread_args;


/* Structure d'en-tête pour notifier la carte worker de la taille des informations. */
typedef struct messageHeader {
    int matrixSize;
    int numRows;
    int matrixOffset;
    int priority;
} messageHeader;

/* Timers pour le benchmark. */
clock_t begin;
clock_t end;

/* Matrices à envoyer sur le réseau. */
int *matrixA;
int *matrixB;

/* Taille des matrices. */
int matrixSize;
/* Nombre de cartes connectées sur le réseau. */
int numConnectedCards;
/* Index de la dernière carte connectée. */
int lastCardIndex = 0;


/** Méthode helper pour afficher une matrice à la console.
 * @param matrix La matrice a afficher
 * @param size La taille de la matrice
*/
void printMatrix(int* matrix, int size) {
    for (int k = 0; k < size * size; k++) {
        printf("%d\t", matrix[k]);
        if (k != 0 && (k % size) == (size - 1)) {
            printf("\n");
        }
    }
}


/** Méthode helper pour initialiser les matrices avec
 * des coefficients aléatoires.
 * @param matrixSize La taille des matrices
 */
void initMatrices(int matrixSize) {
    matrixA = malloc(matrixSize * matrixSize * sizeof(int));
    matrixB = malloc(matrixSize * matrixSize * sizeof(int));

    for (int i = 0; i < matrixSize; i++) {
        for (int j = 0; j < matrixSize; j++) {
            matrixA[i * matrixSize + j] = rand();
            matrixB[i * matrixSize + j] = rand();
        }
    }
}


/** Méthode helper afin d'envoyer un bloc d'information à la carte worker.
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

    /* 4. Envoyer l'en-tête, la matrice A par lignes puis la matrice B en entière. */
    
    /* Instantiation du header. */
    struct messageHeader header;
    header.matrixSize = matrixSize;
    header.numRows = matrixSize / numConnectedCards;
    header.matrixOffset = arg->connectedIndex * header.numRows * header.matrixSize;
    header.priority = arg->priority;

    if (arg->connectedIndex == (numConnectedCards - 1)) {
        header.numRows = header.numRows + (matrixSize % numConnectedCards);
    }

    /* Mise à jour des informations récupérables en dehors du thread. */
    arg->numRows = header.numRows;
    arg->matrixOffset = header.matrixOffset;

    /* On envoie le header réseau. */
    if ((sendMessage(clientSocket, (const char *) &header, sizeof(header))) == -1) {
        printf("ERREUR : L'envoi du header à orin-nano-%d s'est mal déroulé\n", arg->index);
        goto end_connection;
    }

    /* On envoie la matrice A par lignes. */
    if ((sendMessage(clientSocket, (const char *) (matrixA + header.matrixOffset), header.numRows * matrixSize * sizeof(int))) == -1) {
        printf("ERREUR : L'envoi de la matrice A à orin-nano-%d s'est mal déroulé\n", arg->index);
        goto end_connection;
    }

    /* On envoie la matrice B en entier. */
    if ((sendMessage(clientSocket, (const char *) matrixB, matrixSize * matrixSize * sizeof(int))) == -1) {
        printf("ERREUR : L'envoi de la matrice B à orin-nano-%d s'est mal déroulé\n", arg->index);
        goto end_connection;
    }
    
    /* 5. Attendre la réception des résultats pour reconstruire la matrice C. */
    int receivedBytes = 0;
    int bytesToReceive = header.numRows * matrixSize * sizeof(int);
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
 * @param args Les paramètres de thread contenant les informations sur l'adresse ip des cartes
 * @param connectedCards Le nombre de cartes connectées et découvertes
 */
void checkConnectedJetsons(pthread_args *args, int* connectedCards) {
    printf("\n\n\n==========================================================\n");
    printf("Détection des cartes présentes sur le réseau...\n");

    struct fd_set socketWriteSet;
    FD_ZERO(&socketWriteSet);

    TIMEVAL timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 500000;

    SOCKET clientSockets[NUM_CARDS];
    int maxSocketFd = 0;

    for (int i = 0; i < NUM_CARDS; i++) {
        /* Récuperation de la structure contenant des informations. */
        pthread_args *arg = &args[i];
        /* Créer un socket pour tester la connexion à une jetson nano. */
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

        setNonBlocking(clientSocket);

        connect(clientSocket, (struct sockaddr *) &serverAddress, sizeof(serverAddress));
        clientSockets[i] = clientSocket;
        FD_SET(clientSockets[i], &socketWriteSet);

        if ((int) clientSocket > maxSocketFd) maxSocketFd = clientSockets[i];
    }
    
    select(maxSocketFd + 1, NULL, &socketWriteSet, NULL, &timeout);

    for (int i = 0; i < NUM_CARDS; i++) {
        if (FD_ISSET(clientSockets[i], &socketWriteSet) != 0) {
            connectedCards[i] = 1;
            printf("La orin-nano-%d est présente sur le réseau.\n", i);
        } else {
            printf("La orin-nano-%d n'est pas présente sur le réseau.\n", i);
            connectedCards[i] = 0;
        }

        CLOSE_SOCKET(clientSockets[i]);
    }
    printf("==========================================================\n\n");
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
int startMain(int _matrixSize, int priority) {
    numConnectedCards = 0;
    init_connection();

    matrixSize = _matrixSize;

    /* 1. Créer les deux matrices à calculer, A et B de taille NxN. */
    initMatrices(matrixSize);
    
    /* 2. Créer NUM_CARDS thread pour gérer les sockets et le résultat de chaque worker. */
    begin = clock();
    pthread_t *threads = malloc(NUM_CARDS * sizeof(pthread_t));
    pthread_args *args = malloc(NUM_CARDS * sizeof(pthread_args));

    /* Initialisation des arguments des threads. */
    for (int n = 0; n < NUM_CARDS; n++) {
        args[n].index = n;
        args[n].result = NULL;
        args[n].numRows = 0;
        args[n].matrixOffset = 0;
        args[n].priority = priority;
    }

    int* connectedCards = malloc(NUM_CARDS * sizeof(int));
    checkConnectedJetsons(args, connectedCards);

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
        pthread_create(&threads[n], NULL, threadMain, &args[n]);
    }

    /* Attente de la terminaison des threads. */
    for (int n = 0; n < NUM_CARDS; n++) {
        pthread_join(threads[n], NULL);
    }
   
    /* 6. Vérifier qu'il n'y ait pas eu d'erreurs de calcul / race-conditions et mesurer le temps. */
    int *matrixC = malloc(matrixSize * matrixSize * sizeof(int));
    memset(matrixC, 0, matrixSize * matrixSize * sizeof(int));

    for (int i = 0; i < NUM_CARDS; i++) {
        if (args[i].result == NULL && connectedCards[i] == 1) {
            printf("ERREUR : Il manque les données de la carte orin-nano-%d\n", i);
            continue;
        }
        memcpy(matrixC + args[i].matrixOffset, args[i].result, args[i].numRows * matrixSize * sizeof(int));
    }
    end = clock();
    
    printf("\n==========================================================\n");
    printf("BENCHMARK : Temps de calcul + transit réseau pour les jetson nano : %f\n", (double)(end - begin) / CLOCKS_PER_SEC);
    
    /* Ecriture des résultat dans le fichier csv. */
    int csvFile;
    char fileName[30];
    sprintf(fileName, "resultatsGPU/result%dcard.csv", numConnectedCards);

    if ((csvFile = open(fileName, O_CREAT | O_WRONLY | O_APPEND, 0644)) == -1) {
        printf("ERREUR : Erreur lors de l'ouverture du fichier csv.\n");
        close(csvFile);
    }

    /* Variable pour l'écriture des données dans le fichier csv. */
    char stringToWrite[20];
    int writtenBytes = 0;
    writtenBytes = sprintf(stringToWrite, "%d;", matrixSize);
    write(csvFile, stringToWrite, writtenBytes);

    stringToWrite[0] = '\0';
    writtenBytes = sprintf(stringToWrite, "%f\n", (double)(end - begin) / CLOCKS_PER_SEC);
    write(csvFile, stringToWrite, writtenBytes);
    // printMatrix(matrixC, matrixSize);

    /* Calcul de la matrice depuis le client, pour vérifier le résultat. */
    int *cpuMatrixC = malloc (matrixSize * matrixSize * sizeof(int));

    /* Utilisation de la transposée de B afin d'éviter les cache miss. */
    begin = clock();
    int *transposedMatrixB = malloc(matrixSize * matrixSize * sizeof(int));

    /* Transposition de la matrice B. */
    for (int i = 0; i < matrixSize; i++) {
        for (int j = 0; j < matrixSize; j++) {
            transposedMatrixB[i * matrixSize + j] = matrixB[j * matrixSize + i];
        }
    }

    /* Calcul de C. */
    // for (int i = 0; i < matrixSize; i++) {
    //     for (int j = 0; j < matrixSize; j++) {
    //         int coefficient = 0;
    //         for (int k = 0; k < matrixSize; k++) {
    //             coefficient += matrixA[i * matrixSize + k] * transposedMatrixB[j * matrixSize + k];
    //         }
    //         cpuMatrixC[i * matrixSize + j] = coefficient;
    //     }
    // }

    // end = clock();

    // printf("BENCHMARK : Temps de calcul CPU de la matrice C avec B transposée : %f\n", (double)(end - begin) / CLOCKS_PER_SEC);
    
    // stringToWrite[0] = '\0';
    // writtenBytes = sprintf(stringToWrite, "%f\n", (double)(end - begin) / CLOCKS_PER_SEC);
    // write(csvFile, stringToWrite, writtenBytes);
    // // printMatrix(cpuMatrixC, matrixSize);

    /* Vérification du résultat obtenu. */
    // float erreurTotale = 0;

    // for (int i = 0; i < matrixSize; i++) {
    //     for (int j = 0; j < matrixSize; j++) {
    //         erreurTotale += (float) abs((cpuMatrixC[i * matrixSize + j] - matrixC[i * matrixSize + j]) / cpuMatrixC[i * matrixSize + j]);
    //     }
    // }
    // erreurTotale = (erreurTotale * 100) / (matrixSize * matrixSize);
    printf("==========================================================\n\n");
    // printf("Après vérification du calcul effectué par les jetson, l'erreur relative moyenne est de %.2f%%\n", erreurTotale);
    
    /* Fermeture de la connexion (windows) et libération de la mémoire allouée. */
    close_connection();
    close(csvFile);
    free(matrixA);
    free(matrixB);
    free(matrixC);
    free(cpuMatrixC);
    free(transposedMatrixB);
    free(connectedCards);
    for (int n = 0; n < NUM_CARDS; n++) {
        free(args[n].ipAddress);
        if (args[n].result) {
            free(args[n].result);
        }
    }
    free(threads);
    free(args);
    return 0;
}
