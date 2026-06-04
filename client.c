/* Objectif, distribuer le calcul matriciel C = A x B entre les jetsons. */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>

/* Instructions préprocesseur pour différencier les architectures linux de windows qui ont des imports différents. */
#ifdef _WIN32
    #include <winsock2.h>

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

    typedef int socket_t;
    #define CLOSE_SOCKET close

    void init_connection() {};
    void close_connection() {};
#endif

/* Structure pour envoyer et récupérer les informations par le socket. */
typedef struct pthread_args {
    int index;
    int* result;
    int numRows;
    int matrixOffset;
    char* ipAddress;
} pthread_args;


/* Structure d'en-tête pour notifier la carte worker de la taille des informations. */
typedef struct messageHeader {
    int matrixSize;
    int numRows;
    int matrixOffset;
} messageHeader;


/* Matrices à envoyer sur le réseau. */
int *matrixA;
int *matrixB;

/* Taille des matrices. */
int matrixSize;
/* Nombre de jetson nano. */
int numCards;


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
        printf("Connexion échouée à la jetson nano : %s\n", arg->ipAddress);
        goto end_connection;
    }
    printf("Connexion réussie à la jetson nano : %s\n", arg->ipAddress);

    /* 4. Envoyer l'en-tête, la matrice A par lignes puis la matrice B en entière. */
    
    /* Instantiation du header. */
    struct messageHeader header;
    header.matrixSize = matrixSize;
    header.numRows = matrixSize / numCards;
    header.matrixOffset = arg->index * header.numRows * header.matrixSize;
    if (arg->index == (numCards - 1)) {
        header.numRows = header.numRows + (matrixSize % numCards);
    }
    /* Mise à jour des informations récupérables en dehors du thread. */
    arg->numRows = header.numRows;
    arg->matrixOffset = header.matrixOffset;

    /* On envoie le header réseau. */
    if ((sendMessage(clientSocket, (const char *) &header, sizeof(header))) == -1) {
        printf("L'envoi du header à : %s s'est mal déroulé\n", arg->ipAddress);
        goto end_connection;
    }
    printf("Confirmation de l'envoi du header à : %s\n", arg->ipAddress);

    /* On envoie la matrice A par lignes. */
    if ((sendMessage(clientSocket, (const char *) (matrixA + header.matrixOffset), header.numRows * matrixSize * sizeof(int))) == -1) {
        printf("L'envoi de la matrice A à : %s s'est mal déroulé\n", arg->ipAddress);
        goto end_connection;
    }
    printf("Confirmation de l'envoi de la matrice A à : %s\n", arg->ipAddress);

    /* On envoie la matrice B en entier. */
    if ((sendMessage(clientSocket, (const char *) matrixB, matrixSize * matrixSize * sizeof(int))) == -1) {
        printf("L'envoi de la matrice B à : %s s'est mal déroulé\n", arg->ipAddress);
        goto end_connection;
    }
    printf("Confirmation de l'envoi de la matrice B à : %s\n", arg->ipAddress);
    
    /* 5. Attendre la réception des résultats pour reconstruire la matrice C. */
    int receivedBytes = 0;
    int bytesToReceive = header.numRows * matrixSize * sizeof(int);
    int bytesLeft = bytesToReceive;
    arg->result = malloc(bytesToReceive); 
    while (receivedBytes < bytesToReceive) {
        int bytesReceived = (recv(clientSocket, (char*) arg->result + receivedBytes, bytesLeft, 0));
        if (bytesReceived <= 0) {
            printf("Echec lors de la réception du résultat depuis : %s", arg->ipAddress);
            goto end_connection;
        }
        receivedBytes += bytesReceived;
        bytesLeft -= bytesReceived;
    }
    end_connection:
    CLOSE_SOCKET(clientSocket);
    return NULL;
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
int main(int argc, char *argv[]) {
    init_connection();
    /* 0. Vérification de l'input utilisateur. */
    if (argc < 2) {
        printf("ERREUR : Spécifiez une taille de matrice dans l'appel.\n");
        exit(EXIT_FAILURE);
    }

    if ((matrixSize = atoi(argv[1])) == 0) {
        printf("ERREUR : Veuillez saisir un entier pour la taille.\n");
        exit(EXIT_FAILURE);
    }

    if ((numCards = atoi(argv[2])) == 0) {
        printf("ERREUR : Veuillez saisir un entier pour le nombre de cartes connectées.\n");
        exit(EXIT_FAILURE);
    }

    /* 1. Créer les deux matrices à calculer, A et B de taille NxN. */
    initMatrices(matrixSize);
    
    /* 2. Créer numCards thread pour gérer les sockets et le résultat de chaque worker. */
    pthread_t *threads = malloc(numCards * sizeof(pthread_t));
    pthread_args *args = malloc(numCards * sizeof(pthread_args));

    /* Création des threads.*/
    for (int n = 0; n < numCards; n++) {
        args[n].index = n;
        args[n].result = NULL;
        args[n].numRows = 0;
        args[n].matrixOffset = 0;
        pthread_create(&threads[n], NULL, threadMain, &args[n]);
    }

    /* Attente de la terminaison des threads. */
    for (int n = 0; n < numCards; n++) {
        pthread_join(threads[n], NULL);
    }
    
    /* 6. Vérifier qu'il n'y ait pas eu d'erreurs de calcul / race-conditions et mesurer le temps. */
    int *matrixC = malloc(matrixSize * matrixSize * sizeof(int));

    for (int i = 0; i < numCards; i++) {
        if (args[i].result == NULL) {
            printf("Il manque les données de la carte orin-nano-%d\n", i);
            continue;
        }
        memcpy(matrixC + args[i].matrixOffset, args[i].result, args[i].numRows * matrixSize * sizeof(int));
    }

    /* Calcul de la matrice depuis le client, pour vérifier le résultat. */
    int *cpuMatrixC = malloc (matrixSize * matrixSize * sizeof(int));

    for (int i = 0; i < matrixSize; i++) {
        for (int j = 0; j < matrixSize; j++) {
            int coefficient = 0;
            for (int k = 0; k < matrixSize; k++) {
                coefficient += matrixA[i * matrixSize + k] * matrixB[k * matrixSize + j];
            }
            cpuMatrixC[i * matrixSize + j] = coefficient;
        }
    }

    /* Vérification du résultat obtenu. */
    float erreurTotale = 0;

    for (int i = 0; i < matrixSize; i++) {
        for (int j = 0; j < matrixSize; j++) {
            erreurTotale += (float) abs(cpuMatrixC[i * matrixSize + j] - matrixC[i * matrixSize + j]);
        }
    }
    erreurTotale = erreurTotale / (matrixSize * matrixSize);
    printf("Après vérification du calcul effectué par les jetson, l'erreur absolue moyenne est de %f%%\n", erreurTotale);

    /* Fermeture de la connexion (windows) et libération de la mémoire allouée. */
    close_connection();
    free(matrixA);
    free(matrixB);
    free(matrixC);
    for (int n = 0; n < numCards; n++) {
        free(args[n].ipAddress);
        if (args[n].result) {
            free(args[n].result);
        }
    }
    free(threads);
    free(args);
    return 0;
}