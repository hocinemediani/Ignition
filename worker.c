/* Objectif, calculer les numRows lignes de la matrice C puis les envoyer au serveur. */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>


typedef struct messageHeader {
    int matrixSize;
    int numRows;
    int matrixOffset;
    int priority;
} messageHeader;


int receiveMessage(int clientSocketFd, void *messageToReceive, int size) {
    int receivedBytes = 0;
    int bytesToReceive = size;
    int bytesLeft = bytesToReceive;
    while (receivedBytes < bytesToReceive) {
        int bytesReceived = (recv(clientSocketFd, messageToReceive + receivedBytes, bytesLeft, 0));
        if (bytesReceived <= 0) {
            return -1;
        }
        receivedBytes += bytesReceived;
        bytesLeft -= bytesReceived;
    }
    return 0;
}


void printMatrix(int* matrix, int size) {
    for (int k = 0; k < size * size; k++) {
        printf("%d\t", matrix[k]);
        if (k != 0 && (k % size) == (size - 1)) {
            printf("\n");
        }
    }
}


int main(int argc, char* argv[]) {
    /* 0. Vérification de l'input utilisateur. */
    int numBlocks = 0;
    int blockSize = 0;
    
    if (argc < 3) {
        printf("ERREUR : Spécifiez un nombre de blocks et une taille de block.\n");
        exit(EXIT_FAILURE);
    }

    if ((numBlocks = atoi(argv[1])) == 0) {
        printf("ERREUR : Veuillez saisir un entier pour la taille.\n");
        exit(EXIT_FAILURE);
    }

    if ((blockSize = atoi(argv[2])) == 0) {
        printf("ERREUR : Veuillez saisir un entier pour le nombre de cartes connectées.\n");
        exit(EXIT_FAILURE);
    }

    /* 1. Mettre en place le socket pour la communication réseau et accepter les connexions entrantes. */
    int socketFd;

    if ((socketFd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        printf("Erreur lors de la création du socket.\n");
        exit(EXIT_FAILURE);
    }

    int port = 5798;
    struct sockaddr_in socketAddress;
    socketAddress.sin_family = AF_INET;
    socketAddress.sin_port = htons(port);
    socketAddress.sin_addr.s_addr = INADDR_ANY;

    if (bind(socketFd, (const struct sockaddr *) &socketAddress, sizeof(socketAddress)) == -1) {
        printf("Erreur lors du bind du socket.\n");
        exit(EXIT_FAILURE);
    }
    printf("Bind réalisé avec succès.\n");

    if (listen(socketFd, 10) == -1) {
        printf("Erreur lors de l'écoute sur le socket.\n");
        exit(EXIT_FAILURE);
    }

    while(1) {
        printf("Ecoute sur le port : %d\n", port);
        int clientSocketFd;
        socklen_t addressLength = sizeof(struct sockaddr_in);

        if ((clientSocketFd = accept(socketFd,(struct sockaddr *) &socketAddress, &addressLength)) == -1) {
            printf("Problème lors de l'initialisation de la connexion client/serveur.\n");
            continue;
        }

        /* 2. Récupérer le header, les lignes de la matrice A et la matrice B. */
        struct messageHeader header;

        if ((receiveMessage(clientSocketFd, &header, sizeof(header))) == -1) {
            continue;
        }

        int *matrixA = malloc(header.numRows * header.matrixSize * sizeof(int));

        if ((receiveMessage(clientSocketFd, matrixA, header.numRows * header.matrixSize * sizeof(int))) == -1) {
            printf("Echec lors de la réception des lignes de la matrice A\n");
            continue;
        }

        int *matrixB = malloc(header.matrixSize * header.matrixSize * sizeof(int));

        if ((receiveMessage(clientSocketFd, matrixB, header.matrixSize * header.matrixSize * sizeof(int))) == -1) {
            printf("Echec lors de la réception de la matrice B\n");
            continue;
        }

        /* 3. Calculer les lignes de la matrice C correspondantes. */
        int *matrixC = malloc(header.numRows * header.matrixSize * sizeof(int));

        int *transposedMatrixB = malloc(header.matrixSize * header.matrixSize * sizeof(int));

        for (int i = 0; i < header.matrixSize; i++) {
            for (int j = 0; j < header.matrixSize; j++) {
                transposedMatrixB[i * header.matrixSize + j] = matrixB[j * header.matrixSize + i];
            }
        }

        for (int i = 0; i < header.numRows; i++) {
            for (int j = 0; j < header.matrixSize; j++) {
                int coefficient = 0;
                for (int k = 0; k < header.matrixSize; k++) {
                    coefficient += matrixA[i * header.matrixSize + k] * transposedMatrixB[j * header.matrixSize + k];
                }
                matrixC[i * header.matrixSize + j] = coefficient;
            }
        }

        /* 4. Envoyer les lignes sur le réseau et se remettre en attente d'une connexion. */
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

        printf("Confirmation de l'envoi de la matrice C.\n");
        
        free(matrixA);
        free(matrixB);
        free(matrixC);
        free(transposedMatrixB);
        close(clientSocketFd);
    }
    close(socketFd);
    return 0;
}
