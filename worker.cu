/* Objectif, calculer les numRows lignes de la matrice C puis les envoyer au serveur. */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>


/* Structure d'en-tête pour notifier la carte worker de la taille des informations. */
typedef struct messageHeader {
    int matrixSize;
    int numRows;
    int matrixOffset;
} messageHeader;


/* Timers pour le benchmark. */
clock_t begin;
clock_t end;

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


/** Méthode helper pour initialiser les matrices avec
 * des coefficients aléatoires.
 * @param matrixSize La taille des matrices
 */
void printMatrix(int* matrix, int size) {
    for (int k = 0; k < size * size; k++) {
        printf("%d\t", matrix[k]);
        if (k != 0 && (k % size) == (size - 1)) {
            printf("\n");
        }
    }
}


/** Fonction helper permettant de calculer le produit C = AxB et de
 * mettre le résultat dans C.
 * @param matrixSize La taille des matrices
 * @param numRows Le nombre de lignes de A que l'on à
 * @param matrixA La matrice A
 * @param matrixB La matrice B
 * @param matrixC La matrice C à calculer
 */
__global__
void computeMatrices(int matrixSize, int numRows, int *matrixA, int *matrixB, int *matrixC) {
    /* Numéro du thread actuel. */
    int index = blockIdx.x * blockDim.x + threadIdx.x;
    int totalCoefficients = matrixSize * numRows;

    if (index >= totalCoefficients) {
        return;
    }

    /* A quelle ligne de la matrice le coefficient de ce thread se trouve t'il. */
    int row = index / matrixSize;
    /* A quelle colonne de la matrice le coefficient de ce thread se trouve t'il. */
    int col = index % matrixSize;

    int coefficient = 0;
    /* A i et j fixé, on parcours la ligne de A et la colonne de B. */
    for (int k = 0; k < matrixSize; k++) {
        coefficient += matrixA[row * matrixSize + k] * matrixB[k * matrixSize + col];
    }

    matrixC[index] = coefficient;
}


int main(int argc, char* argv[]) {
    /* 0. Vérification de l'input utilisateur. */
    int blockSize = 0;
    
    if (argc < 2) {
        printf("ERREUR : Spécifiez une taille de block.\n");
        exit(EXIT_FAILURE);
    }

    if ((blockSize = atoi(argv[1])) == 0) {
        printf("ERREUR : Veuillez saisir un entier pour la taille d'un block.\n");
        exit(EXIT_FAILURE);
    }

    /* 1. Mettre en place le socket pour la communication réseau et accepter les connexions entrantes. */
    int socketFd;

    if ((socketFd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        printf("Erreur lors de la création du socket.\n");
        exit(EXIT_FAILURE);
    }

    /* Configuration du socket. */
    int port = 5798;
    struct sockaddr_in socketAddress;
    socketAddress.sin_family = AF_INET;
    socketAddress.sin_port = htons(port);
    socketAddress.sin_addr.s_addr = INADDR_ANY;

    if (bind(socketFd, (const struct sockaddr *) &socketAddress, sizeof(socketAddress)) == -1) {
        printf("Erreur lors du bind du socket.\n");
        exit(EXIT_FAILURE);
    }

    if (listen(socketFd, 10) == -1) {
        printf("Erreur lors de l'écoute sur le socket.\n");
        exit(EXIT_FAILURE);
    }

    /* On écoute constamment pour pouvoir traiter des demandes à la chaine. */
    while(1) {
        printf("Ecoute sur le port : %d\n", port);

        int clientSocketFd;
        socklen_t addressLength = sizeof(struct sockaddr_in);

        if ((clientSocketFd = accept(socketFd,(struct sockaddr *) &socketAddress, &addressLength)) == -1) {
            printf("Problème lors de l'initialisation de la connexion client/serveur.\n");
            continue;
        }

        begin = clock();

        /* 2. Récupérer le header, les lignes de la matrice A et la matrice B. */
        struct messageHeader header;

        if ((receiveMessage(clientSocketFd, &header, sizeof(header))) == -1) {
            printf("Echec lors de la réception du header\n");
            continue;
        }

        int *matrixA;
        cudaMallocManaged(&matrixA, header.numRows * header.matrixSize * sizeof(int));

        if ((receiveMessage(clientSocketFd, matrixA, header.numRows * header.matrixSize * sizeof(int))) == -1) {
            printf("Echec lors de la réception des lignes de la matrice A\n");
            continue;
        }

        int *matrixB;
        cudaMallocManaged(&matrixB, header.matrixSize * header.matrixSize * sizeof(int));

        if ((receiveMessage(clientSocketFd, matrixB, header.matrixSize * header.matrixSize * sizeof(int))) == -1) {
            printf("Echec lors de la réception de la matrice B\n");
            continue;
        }

        /* 3. Calculer les lignes de la matrice C correspondantes. */
        int *matrixC;
        cudaMallocManaged(&matrixC, header.numRows * header.matrixSize * sizeof(int));

        int numBlocks = (header.matrixSize * header.numRows + blockSize - 1) / blockSize;
        
        /* On copie en VRAM les données utiles. */
        cudaMemPrefetchAsync(matrixA, header.numRows * header.matrixSize * sizeof(int), 0, 0);
        cudaMemPrefetchAsync(matrixB, header.matrixSize * header.matrixSize * sizeof(int), 0, 0);
        cudaMemPrefetchAsync(matrixC, header.matrixSize * header.matrixSize * sizeof(int), 0, 0);

        computeMatrices<<<numBlocks, blockSize>>>(header.matrixSize, header.numRows, matrixA, matrixB, matrixC);

        /* On attends que le GPU ait fini les tâches assignées. */
        cudaDeviceSynchronize();
        end = clock();

        /* 4. Envoyer les lignes sur le réseau et se remettre en attente d'une connexion. */

        printf("Fin des calculs, début de l'envoi.\n");

        /* Envoi sur le réseau. */
        int sentBytes = 0;
        int size = header.numRows * header.matrixSize * sizeof(int);
        int bytesToSend = size;
        while (sentBytes < size) {
            int bytesSent = (send(clientSocketFd, (char *)matrixC + sentBytes, bytesToSend, 0));
            if (bytesSent == -1) {
                printf("Echec lors de l'envoi de la matrice C\n");
                break;
            }
            sentBytes += bytesSent;
            bytesToSend -= bytesSent;
        }
        
        cudaFree(matrixA);
        cudaFree(matrixB);
        cudaFree(matrixC);
        close(clientSocketFd);
    }
    close(socketFd);
    return 0;
}
