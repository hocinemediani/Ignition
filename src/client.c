#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <libgen.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <signal.h>

#define STB_IMAGE_IMPLEMENTATION
#include "../include/stb_image.h"

/* gcc -Wall -Wextra -I../include -L../lib client.c -o client -lmingw32 -lSDL2main -lSDL2 -lws2_32 */

/* Instructions préprocesseur pour différencier les architectures linux de windows qui ont des imports différents. */
#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
    #include <process.h>
    #include <io.h>
    #define SDL_MAIN_HANDLED
    #include <SDL2/SDL.h>

    typedef SOCKET socket_t;
    #define CLOSE_SOCKET closesocket

    /** Initialise les sockets sous Windows. */
    void init_connection() {
        WSADATA wsadata;
        WSAStartup(MAKEWORD(2, 2), &wsadata);
    }

    /** Ferme proprement l'API des sockets sous Windows. */
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

    /** Initialise les sockets (vide sous Linux). */
    void init_connection() {};
    /** Ferme l'environnement socket (vide sous Linux). */
    void close_connection() {};
#endif

/** Reçoit un message depuis un socket client.
 * @param clientSocketFd Le descripteur de fichier du socket client
 * @param messageToReceive Un pointeur vers le buffer stockant le message reçu
 * @param size La taille du message attendu
 * @return 0 si la réception s'est bien déroulée, -1 sinon
 */
int receiveMessage(socket_t clientSocketFd, void *messageToReceive, int size) {
    int receivedBytes = 0;
    int bytesToReceive = size;
    int bytesLeft = bytesToReceive;
    
    /* Boucle de réception bloquante jusqu'à ce que la taille demandée soit atteinte. */
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


/** Point d'entrée du programme initialisant les composants SDL et la communication réseau.
 * @param argc Le nombre d'arguments en ligne de commande
 * @param argv Le tableau contenant les arguments
 * @return EXIT_SUCCESS lors de la fin de l'exécution
 */
int main(int argc, char *argv[]) {
    init_connection();

    /* Vérification de l'argument utilisateur pour l'index de la carte. */
    if (argc != 2) {
        printf("ERREUR : Veuillez renseigner l'index de la carte à laquelle vous souhaitez vous connecter.\n");
        exit(EXIT_FAILURE);
    }

    /* Récupération de l'index de la worker et création de son adresse IP. */
    int workerIndex = atoi(argv[1]);
    char workerIp[32];
    snprintf(workerIp, sizeof(workerIp), "147.127.121.%d", 94 + workerIndex);

    /* Création du socket TCP pour se connecter à la Jetson. */
    socket_t clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket == INVALID_SOCKET) {
        printf("ERREUR : Le socket n'a pas pu être créé.\n");
        exit(EXIT_FAILURE);
    }

    /* Configuration des paramètres d'adresse et de port pour la connexion. */
    struct sockaddr_in workerAddress;
    memset(&workerAddress, 0, sizeof(workerAddress));
    workerAddress.sin_addr.s_addr = inet_addr(workerIp);
    workerAddress.sin_family = AF_INET;
    workerAddress.sin_port = htons(9988);

    /* Tentative de connexion au serveur de la carte Jetson. */
    if (connect(clientSocket, (const struct sockaddr *) &workerAddress, sizeof(workerAddress)) == -1) {
        printf("ERREUR : La connexion est impossible à la carte d'IP : %s", workerIp);
        exit(EXIT_FAILURE);
    }
    printf("Connexion à la carte réussie avec succès.\n");

    /* Initialisation des sous-systèmes vidéo de SDL. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("ERREUR : SDL n'a pas pu être initialisé.\n");
    }

    /* Création de la fenêtre principale d'affichage. */
    SDL_Window *window = SDL_CreateWindow("Flux Jetson Orin Nano", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 640, 0);
    if (window == NULL) {
        printf("ERREUR : La fenêtre d'affichage n'a pas pu être créé.\n");
        exit(EXIT_FAILURE);
    }

    /* Création du renderer associé à la fenêtre pour le rendu. */
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_TARGETTEXTURE);
    if (renderer == NULL) {
        printf("ERREUR : Le renderer de la fenêtre n'a pas pu être créé.\n");
        exit(EXIT_FAILURE);
    }

    /* Création de la texture qui contiendra les frames vidéo décodées. */
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 640, 640);
    if (texture == NULL) {
        printf("ERREUR : La texture n'a pas pu être créée.\n");
        exit(EXIT_FAILURE);
    }

    SDL_Event event;
    
    /* Boucle infinie traitant la réception et l'affichage des frames. */
    while (1) {
        /* Récupération de la taille du tableau des coordonnées (x, y). */
        int coordinateSize;
        if (receiveMessage(clientSocket, &coordinateSize, sizeof(coordinateSize)) == -1) break;
        int coordinateListSize = ntohl(coordinateSize);

        /* Allocation dynamique et réception des coordonnées des détections. */
        int *coordinateList = (int *) malloc(coordinateListSize);
        if ((receiveMessage(clientSocket, coordinateList, coordinateListSize)) == -1) break;

        /* Récupération de la taille de la liste des noms des objets détectés. */
        int listSize;
        if (receiveMessage(clientSocket, &listSize, sizeof(listSize)) == -1) break;
        int detectionSize = ntohl(listSize);

        /* Allocation dynamique et réception des chaînes de caractères des détections. */
        char (*detectionList)[64] = (char(*)[64]) malloc(detectionSize);
        if (detectionList == NULL) exit(EXIT_FAILURE);
        if (receiveMessage(clientSocket, detectionList, detectionSize) == -1) break;

        /* Récupération de la taille de l'image (jpeg) puis de l'image. */
        uint32_t size;
        receiveMessage(clientSocket, &size, sizeof(size));
        uint32_t imageSize = ntohl(size);
        
        /* Allocation dynamique et réception du payload de l'image. */
        void* receivedImage = malloc(imageSize);
        receiveMessage(clientSocket, receivedImage, imageSize);

        /* Dépilement des évènements SDL pour intercepter la demande de fermeture. */
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                exit(EXIT_SUCCESS);
            }
        }

        int imageWidth;
        int imageHeight;
        int comp;
        
        /* Décompression du buffer JPEG reçu vers un tableau de pixels (RGBA). */
        stbi_uc *pixelArray = stbi_load_from_memory((const stbi_uc *) receivedImage, imageSize, &imageWidth, &imageHeight, &comp, 4);

        /* Mise à jour de la texture SDL avec les nouveaux pixels décompressés. */
        SDL_UpdateTexture(texture, NULL, pixelArray, imageWidth * 4);
        stbi_image_free(pixelArray);

        /* Nettoyage du renderer puis copie de la texture pour le rafraîchissement à l'écran. */
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        /* Affichage dans la console de chaque objet détecté et de sa position. */
        for (int i = 0; i < (int) (coordinateListSize / (4 * sizeof(uint32_t))); i++) {
            printf("Détection de : %s à (%d, %d).\n", detectionList[i], coordinateList[2 * i], coordinateList[2 * i + 1]);
        }

        /* Libération des ressources mémoire allouées pour la frame courante. */
        free(detectionList);
        free(receivedImage);
        free(coordinateList);
    }

    /* Nettoyage final en cas de sortie de la boucle de réception. */
    CLOSE_SOCKET(clientSocket);
    close_connection();
}