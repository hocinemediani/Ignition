#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/* Structure d'en-tête pour notifier de la taille des informations transitant. */
typedef struct messageHeader {
    int messageSize;
    int priority;
    int taskID;
} messageHeader;

/* Structure à envoyer sur le port 9988 pour informer d'un changement d'état de connexion ou de file d'attente. */
typedef struct monitoringMessage {
    /* Possible : HELLO, BYE, INFO. */
    char type[6];
    int sizeLeft;
} monitoringMessage;

/* Booléen pour la terminaison du processus. */
volatile sig_atomic_t isRunning = 1;


void initInteruptHandling() {
    /* Récupération du signal SIGINT pour fermer proprement le socket. */
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    act.sa_handler = sigIntHandler;
    sigaction(SIGINT, &action, NULL);
}


void sigIntHandler(int sig) {
    (void) sig;
    isRunning = 0;
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


int main(int argc, char* argv[]) {
    initInteruptHandling();

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
    while(isRunning == 1) {
        printf("Ecoute sur le port : %d\n", port);

        int clientSocketFd;
        socklen_t addressLength = sizeof(addressLength);

        if ((clientSocketFd = accept(socketFd,(struct sockaddr *) &socketAddress, &addressLength)) == -1) {
            printf("Problème lors de l'initialisation de la connexion client/serveur.\n");
            continue;
        }

        /* 2. Récupérer le header, les lignes de la matrice A et la matrice B. */
        struct messageHeader header;

        if ((receiveMessage(clientSocketFd, &header, sizeof(header))) == -1) {
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

        begin = clock();
        
        end = clock();

        printf("\n==========================================================\n");
        printf("BENCHMARK : Temps de calcul de la matrice C : %f\n", (double)(end - begin) / CLOCKS_PER_SEC);
        printf("==========================================================\n\n");

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
        
        cudaFree(matrixA);
        cudaFree(matrixB);
        cudaFree(matrixC);
        close(clientSocketFd);
    }
    close(socketFd);
    return 0;
}
