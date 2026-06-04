/* Objectif, calculer les numRows lignes de la matrice C puis les envoyer au serveur. */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

typedef struct messageHeader {
    int matrixSize;
    int numRows;
    int matrixOffset;
} messageHeader;

/* 1. Mettre en place le socket pour la communication réseau. */
/* 2. Récupérer*/