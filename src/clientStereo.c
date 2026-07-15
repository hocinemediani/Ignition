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
#include "hashmap.h"

#define STB_IMAGE_IMPLEMENTATION
#include "../include/stb_image.h"

/* gcc -Wall -Wextra -I../include -L../lib clientStereo.c hashmap.c -o clientStereo -lmingw32 -lSDL2main -lSDL2 -lws2_32 */

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

struct threadContext {
    int numBoxes;
    int imageWidth;
    int workerIndex;
    int canGetImage;
    stbi_uc *pixelArray;
    SDL_Texture *texture;
    SDL_Renderer *renderer;
    struct hashMap *hashMap;
    struct hashMap *previousHashMap;
    pthread_mutex_t *imageMutex;
    char(*detectionList)[64];
};

Uint32 eventID;

int hashMapUpdated = 0;
pthread_mutex_t updatedMutex;


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


void* receivingThreadMain(void *_arg) {
    /* Récupération du contexte utile. */
    struct threadContext *context = (struct threadContext *) _arg;

    /* Création de la string d'adresse IP. */
    char workerIp[32];
    snprintf(workerIp, sizeof(workerIp), "147.127.121.%d", 94 + context->workerIndex);

    /* Création du socket de connexion à la jetson. */
    socket_t clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket == INVALID_SOCKET) {
        printf("ERREUR : Le socket n'a pas pu être créé.\n");
        exit(EXIT_FAILURE);
    }

    /* Connexion à la jetson. */
    struct sockaddr_in workerAddress;
    memset(&workerAddress, 0, sizeof(workerAddress));
    workerAddress.sin_addr.s_addr = inet_addr(workerIp);
    workerAddress.sin_family = AF_INET;
    workerAddress.sin_port = htons(9988);

    if (connect(clientSocket, (const struct sockaddr *) &workerAddress, sizeof(workerAddress)) == -1) {
        printf("ERREUR : La connexion est impossible à la carte d'IP : %s", workerIp);
        exit(EXIT_FAILURE);
    }
    printf("Connexion à la carte orin-nano-%d réussie avec succès.\n", context->workerIndex);

    /* Création de l'event pour notifier la fenêtre de la présence d'une nouvelle image. */
    SDL_Event event;
    SDL_zero(event);
    event.type = eventID;
    event.user.code = (Sint32) context->workerIndex;

    while (1) {
        /* Récupération des x_min et y_min. */
        int coordinateSize;
        if (receiveMessage(clientSocket, &coordinateSize, sizeof(coordinateSize)) == -1) break;
        int coordinateListSize = ntohl(coordinateSize);

        int *coordinateList = (int *) malloc(coordinateListSize);
        if ((receiveMessage(clientSocket, coordinateList, coordinateListSize)) == -1) break;

        /* Récupération des détections. */
        int listSize;
        if (receiveMessage(clientSocket, &listSize, sizeof(listSize)) == -1) break;
        int detectionSize = ntohl(listSize);
        
        context->numBoxes = detectionSize / 64;

        char (*detectionList)[64] = (char(*)[64]) malloc(detectionSize);
        if (detectionList == NULL) exit(EXIT_FAILURE);
        if (receiveMessage(clientSocket, detectionList, detectionSize) == -1) break;

        /* Récupération de la taille de l'image et l'image. */
        uint32_t size;
        if (receiveMessage(clientSocket, &size, sizeof(size)) == -1) {
            free(detectionList);
            break;
        }
        uint32_t imageSize = ntohl(size);

        if (imageSize > 5 * 1024 * 1024) {
            free(detectionList);
            break;
        }

        void* receivedImage = malloc(imageSize);
        if (receivedImage == NULL) exit(EXIT_FAILURE);
        if (receiveMessage(clientSocket, receivedImage, imageSize) == -1) {
            free(receivedImage);
            break;
        }

        int imageWidth;
        int imageHeight;
        int comp;

        stbi_uc *pixelArray = stbi_load_from_memory((const stbi_uc *) receivedImage, imageSize, &imageWidth, &imageHeight, &comp, 4);

        /* Récupération du tableau correspondant à l'image. */
        if (pthread_mutex_trylock(context->imageMutex) == 0) {
            struct hashMap *tempHashMap = context->previousHashMap;
            context->previousHashMap = context->hashMap;
            context->hashMap = tempHashMap;
            clearHashMap(context->hashMap);

            if (context->pixelArray != NULL) stbi_image_free(context->pixelArray);
            context->pixelArray = pixelArray;

            if (context->detectionList != NULL) free(context->detectionList);
            context->detectionList = detectionList;

            for (int i = 0; i < context->numBoxes; i++) {
                createNode(context->detectionList[i], coordinateList[2 * i], coordinateList[2 * i + 1], context->hashMap);
            }

            if (nodeExists("tv", context->hashMap)) {
                int offsetX = getX("tv", context->hashMap);
                int offsetY = getY("tv", context->hashMap);
                for (int i = 0; i < context->numBoxes; i++) {
                    updateNode(context->detectionList[i], coordinateList[2 * i], coordinateList[2 * i + 1], coordinateList[2 * i] - offsetX, coordinateList[2 * i + 1] - offsetY, context->hashMap);
                }
            }
            pthread_mutex_lock(&updatedMutex);
            hashMapUpdated = hashMapUpdated | 1 << context->workerIndex;
            pthread_mutex_unlock(&updatedMutex);

            context->imageWidth = imageWidth;
            context->canGetImage = 1;
            SDL_PushEvent(&event);
            pthread_mutex_unlock(context->imageMutex);
        } else {
            free(detectionList);
            stbi_image_free(pixelArray);
        }
        
        free(coordinateList);
        free(receivedImage);
    }

    CLOSE_SOCKET(clientSocket);
    exit(EXIT_FAILURE);
    return NULL;
}


void initializeContext(struct threadContext *context, SDL_Texture *texture, SDL_Renderer *renderer, int workerIndex, pthread_mutex_t *mutex) {
    context->numBoxes = 0;
    context->canGetImage = 0;
    context->pixelArray = NULL;
    context->texture = texture;
    context->imageMutex = mutex;
    context->renderer = renderer;
    context->detectionList = NULL;
    context->workerIndex = workerIndex;
    context->hashMap = initializeHashMap(32);
    context->previousHashMap = initializeHashMap(32);
}


void renderImage(struct threadContext *context) {
    pthread_mutex_lock(context->imageMutex);

    if (context->canGetImage != 1 || context->pixelArray == NULL) {
        pthread_mutex_unlock(context->imageMutex);
        return;
    }

    SDL_UpdateTexture(context->texture, NULL, context->pixelArray, context->imageWidth * 4);
    context->canGetImage = 0;

    pthread_mutex_unlock(context->imageMutex);

    SDL_RenderClear(context->renderer);
    SDL_RenderCopy(context->renderer, context->texture, NULL, NULL);
    SDL_RenderPresent(context->renderer);
}


void printDetections(struct threadContext *context1, struct threadContext *context2) {
    pthread_mutex_lock(context1->imageMutex);
    pthread_mutex_lock(context2->imageMutex);

    if ((compareHashMap(context1->hashMap, context1->previousHashMap) == 0) && (compareHashMap(context2->hashMap, context2->previousHashMap) == 0)) goto unlock_mutexes;

    if (context1->detectionList == NULL || context2->detectionList == NULL) {
        goto unlock_mutexes;
    }

    printf("\n\n\n\n");

    for (int i = 0; i < context1->hashMap->capacity; i++) {
        struct node *toExplore = context1->hashMap->hashTable[i];

        while (toExplore != NULL) {
            int x1 = toExplore->x;
            int y1 = toExplore->y;

            if (x1 == ERROR_COORDINATE || y1 == ERROR_COORDINATE) {
                toExplore = toExplore->next;
                continue;
            }
            
            float deltaX = 640;
            float deltaY = 640;
            
            struct node *camNode = context2->hashMap->hashTable[getHashValue(toExplore->key, context2->hashMap)];
            while (camNode != NULL) {
                if (strcmp(camNode->key, toExplore->key) != 0 || camNode->isSeen == 1) {
                    camNode = camNode->next;
                    continue;
                }

                deltaX = ((float) x1 - camNode->x);
                deltaY = ((float) y1 - camNode->y);
                camNode->isSeen = 1;
                break;
            }

            deltaX = deltaX > 0 ? deltaX : -deltaX;
            deltaY = deltaY > 0 ? deltaY : -deltaY;

            if (deltaX > 0.15 * 640 || deltaY > 0.15 * 640) {
                printf("Détection par orin-nano-%d de %s à (%d, %d).\n", context1->workerIndex, toExplore->key, x1, y1);
                if (camNode != NULL) printf("Détection par orin-nano-%d de %s à (%d, %d).\n", context2->workerIndex, toExplore->key, camNode->x, camNode->y);
            } else {
                printf("Détection mutualisée de %s à (%d, %d).\n", toExplore->key, x1, y1);
            }

            toExplore = toExplore->next;
        }
    }

    /* Affichage des objets détectés par la caméra 2 et non détectés par la caméra 1. */
    for (int i = 0; i < context2->hashMap->capacity; i++) {

        struct node *toExplore = context2->hashMap->hashTable[i];

        while (toExplore != NULL) {
            if (toExplore->isSeen == 0) printf("Détection par orin-nano-%d de %s à (%d, %d).\n", context2->workerIndex, toExplore->key,toExplore->x, toExplore->y);
            toExplore = toExplore->next;
        }
    }

    printf("\n\n\n\n");

    unlock_mutexes:
    pthread_mutex_unlock(context1->imageMutex);
    pthread_mutex_unlock(context2->imageMutex);
}


int main(int argc, char *argv[]) {
    init_connection();

    /* Vérification de l'input utilisateur. */
    if (argc != 3) {
        printf("ERREUR : Veuillez renseigner l'index des cartes auxquelles vous souhaitez vous connecter.\n");
        exit(EXIT_FAILURE);
    }

    /* Connexion aux workers. */
    int workerIndex1 = atoi(argv[1]);
    int workerIndex2 = atoi(argv[2]);

    /* Initialisation des fenêtres de prévisualisation. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("ERREUR : SDL n'a pas pu être initialisé.\n");
        exit(EXIT_FAILURE);
    }
    
    /* Récupération de la taille de l'écran. */
    SDL_Rect screenRect;
    SDL_GetDisplayBounds(0, &screenRect);

    SDL_Window *window = SDL_CreateWindow("Flux 1 Jetson Orin Nano", screenRect.w / 2 - 640, SDL_WINDOWPOS_CENTERED, 640, 640, 0);
    SDL_Window *secondWindow = SDL_CreateWindow("Flux 2 Jetson Orin Nano", screenRect.w / 2, SDL_WINDOWPOS_CENTERED, 640, 640, 0);
    if (window == NULL || secondWindow == NULL) {
        printf("ERREUR : La fenêtre d'affichage n'a pas pu être créé.\n");
        exit(EXIT_FAILURE);
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_TARGETTEXTURE);
    SDL_Renderer *secondRenderer = SDL_CreateRenderer(secondWindow, -1, SDL_RENDERER_TARGETTEXTURE);
    if (renderer == NULL || secondRenderer == NULL) {
        printf("ERREUR : Le renderer de la fenêtre n'a pas pu être créé.\n");
        exit(EXIT_FAILURE);
    }

    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 640, 640);
    SDL_Texture *secondTexture = SDL_CreateTexture(secondRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 640, 640);
    if (texture == NULL || secondTexture == NULL) {
        printf("ERREUR : La texture n'a pas pu être créée.\n");
        exit(EXIT_FAILURE);
    }

    /* Initialisation des mutexes pour les images. */
    pthread_mutex_t firstThreadMutex;
    pthread_mutex_t secondThreadMutex;

    pthread_mutex_init(&firstThreadMutex, NULL);
    pthread_mutex_init(&secondThreadMutex, NULL);
    pthread_mutex_init(&updatedMutex, NULL);

    /* Récupération d'un ID d'event valide. */
    eventID = SDL_RegisterEvents(1);

    /* Création du premier thread d'écoute. */
    struct threadContext firstThreadContext;
    initializeContext(&firstThreadContext, texture, renderer, workerIndex1, &firstThreadMutex);

    pthread_t firstReceivingThread;
    if (pthread_create(&firstReceivingThread, NULL, receivingThreadMain, &firstThreadContext) != 0) {
        printf("ERREUR : Le premier thread d'écoute du flux vidéo n'a pas pu être créé.\n");
        exit(EXIT_FAILURE);
    } else {
        pthread_detach(firstReceivingThread);
    }

    /* Création du second thread d'écoute. */
    struct threadContext secondThreadContext;
    initializeContext(&secondThreadContext, secondTexture, secondRenderer, workerIndex2, &secondThreadMutex);

    pthread_t secondReceivingThread;
    if (pthread_create(&secondReceivingThread, NULL, receivingThreadMain, &secondThreadContext) != 0) {
        printf("ERREUR : Le deuxième thread d'écoute du flux vidéo n'a pas pu être créé.\n");
        exit(EXIT_FAILURE);
    } else {
        pthread_detach(secondReceivingThread);
    }

    SDL_Event event;
    while (1) {
        /* Afficher l'image à l'écran avec SDL. */
        while (SDL_WaitEvent(&event)) {
            if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
                exit(EXIT_SUCCESS);
            }

            int shouldPrint = 0;
            pthread_mutex_lock(&updatedMutex);
            if (hashMapUpdated == (0 | 1 << workerIndex1 | 1 << workerIndex2)) {
                shouldPrint = 1;
                hashMapUpdated = 0;
            }
            pthread_mutex_unlock(&updatedMutex);

            if (shouldPrint) {
                printDetections(&firstThreadContext, &secondThreadContext);
                shouldPrint = 0;
            }

            if (event.type == eventID) {

                if (event.user.code == (Sint32) workerIndex1) {
                    renderImage(&firstThreadContext);
                }

                if (event.user.code == (Sint32) workerIndex2) {
                    renderImage(&secondThreadContext);
                }
            }
        }
    }

    close_connection();
}
