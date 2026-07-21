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
#include <math.h>
#include <SDL2/SDL_ttf.h>
#include "hashmap.h"

#define STB_IMAGE_IMPLEMENTATION
#include "../include/stb_image.h"

/* gcc -Wall -Wextra -I../include -L../lib clientStereo.c hashmap.c -o clientStereo -lmingw32 -lSDL2main -lSDL2 -lSDL2_TTF -lws2_32 */

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

/* Structure contenant le contexte propre à chaque thread. Le contexte est constitué de tout ce qui peut-être
 * utilisé ou récupéré par le thread, afin de s'abstraire de l'index de la caméra ou de l'index du thread. */
struct threadContext {
    /* La coordonnée sur l'axe des abscisses de l'objet de référence. */
    int offsetX;
    /* La coordonnée sur l'axe des ordonnées de l'objet de référence.*/
    int offsetY;
    /* Le nombre de boîtes de détections récupérées par YOLO. */
    int numBoxes;
    /* L'index du thread (0 ou 1), uniquement utilisé pour l'écriture des fichiers d'image de calibration. */
    int index;
    /* La largeur de l'image, utilisée pour savoir le nombre de pixel avant d'arriver sur une nouvelle ligne. */
    int imageWidth;
    /* L'index de la worker à laquelle le thread est rattaché. */
    int workerIndex;
    /* Un flag booléen valant 1 lorsque une image peut-être récupérée par le thread principal pour l'affichage, et 0 sinon. */
    int canGetImage;
    /* Un pointeur vers le tableau contenant les pixels de l'image. */
    stbi_uc *pixelArray;
    /* La texture sur laquelle afficher le contenu du tableau de pixels. */
    SDL_Texture *texture;
    /* Le renderer associé à la texture. */
    SDL_Renderer *renderer;
    /* La hash map contenant les informations sur les détections actuelles. */
    struct hashMap *hashMap;
    /* La hash map contenant les informations sur les détections N-1, pour comparer et ne pas afficher dans
    * le terminal de manière redondante les détections. */
    struct hashMap *previousHashMap;
    /* Le mutex protégeant l'accès aux pixels de l'image. */
    pthread_mutex_t *imageMutex;
    /* Le mutex protégeant l'accès par le script de calibration aux images de calibration. */
    pthread_mutex_t *firstImageMutex;
    /* La variable condition associée au mutex des images de calibration. */
    pthread_cond_t *firstImageCondition;
    /* Un flag booléen valant 1 lorsque la première image est prête et 0 sinon. */
    int firstImageReady;
    /* La liste des strings des détections. */
    char(*detectionList)[64];
};

/* L'identifiant de l'évenement SDL indiquant la possibilité d'un affichage. */
Uint32 eventID;

/* La police d'écriture utilisée pour le rendering des textes. */
TTF_Font *font;

/* Flag booléen valant 1 si la calibration épipolaire est utilisée et 0 sinon. */
int epipolarCalibration = 0;
/* Flag booléen valant 1 si le mode reconstruction de scène est utilisé et 0 sinon. */
int sceneReconstruction = 0;
/* La matrice fondamentale récupérée depuis le script python. */
long double F[3][3];

/* Le mutex protégeant l'accès à la première image du thread 0. */
pthread_mutex_t imageMutex1;
/* Le mutex protégeant l'accès à la première image du thread 1. */
pthread_mutex_t imageMutex2;
/* La variable condition associée au mutex pour le thread 0. */
pthread_cond_t imageCondition1;
/* La variable condition associée au mutex pour le thread 1. */
pthread_cond_t imageCondition2;

/* Variable valant 1 (en binaire) à l'index de la worker associée lorsque la hash map
 * a été mise à jour. */
int hashMapUpdated = 0;
/* Le mutex sécurisant l'accès à la variable `hashMapUpdated`. */
pthread_mutex_t updatedMutex;

/** Ecrit le contenu d'une image reçue dans un fichier.
 * @param imageSize La taille de l'image à écrire
 * @param imageFd Le descripteur de fichier du fichier cible
 * @param receivedImage Le pointeur vers les données de l'image reçue
 * @param imageName Le nom du fichier image pour l'affichage des erreurs
 * @return 0 si l'écriture s'est bien déroulée, -1 sinon
 */
int writeToFile(int imageSize, int imageFd, void *receivedImage, char *imageName) {
    int writtenBytes = 0;
    int bytesToWrite = imageSize;
    
    /* Boucle d'écriture tant que toutes les données n'ont pas été écrites. */
    while (writtenBytes < (int) (imageSize)) {
        int bytesWritten = write(imageFd, receivedImage + writtenBytes, bytesToWrite);
        if (bytesWritten == -1) {
            printf("ERREUR : L'écriture du fichier %s s'est mal déroulée", imageName);
            epipolarCalibration = 0;
            return -1;
        }
        if (bytesWritten == 0) {
            break;
        }
        writtenBytes += bytesWritten;
        bytesToWrite -= bytesWritten;
    }
    printf("Ecriture du fichier %s terminée.\n", imageName);
    close(imageFd);
    return 0;
}


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


/** Point d'entrée pour le thread de réception des données depuis une carte Jetson.
 * @param _arg Un pointeur vers la structure threadContext contenant les informations utiles pour le thread
 * @return NULL lors de la terminaison du thread
 */
void* receivingThreadMain(void *_arg) {
    /* Flag indiquant si la première image a été récupérée ou non. */
    int getFirstImage = 1;

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
        printf("ERREUR : La connexion est impossible à la carte d'IP : %s\n", workerIp);
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

        /* Allocation de la mémoire pour la liste des coordonnées. */
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

        /* Allocation de la mémoire pour l'image reçue. */
        void* receivedImage = malloc(imageSize);
        if (receivedImage == NULL) exit(EXIT_FAILURE);
        if (receiveMessage(clientSocket, receivedImage, imageSize) == -1) {
            free(receivedImage);
            break;
        }

        /* Création du fichier pour l'image de calibration. */
        pthread_mutex_lock(context->firstImageMutex);
        if (epipolarCalibration == 1 && getFirstImage == 1) {
            getFirstImage = 0;

            char imageName[64];
            snprintf(imageName, sizeof(imageName), "./calibration/calibrationImage%d.jpg", context->index);
            int imageFd = open(imageName, O_CREAT | O_RDWR | O_TRUNC |O_BINARY, 0666);

            if (imageFd == -1) {
                epipolarCalibration = 0;
                goto end;
            }

            if (writeToFile(imageSize, imageFd, receivedImage, imageName) == -1) goto end;
            context->firstImageReady = 1;

        }

        end:
        pthread_cond_signal(context->firstImageCondition);
        pthread_mutex_unlock(context->firstImageMutex);

        /* Variables pour stocker les informations de l'image décompressée. */
        int imageWidth;
        int imageHeight;
        int comp;

        /* Décompression de l'image reçue en mémoire vers un tableau de pixels. */
        stbi_uc *pixelArray = stbi_load_from_memory((const stbi_uc *) receivedImage, imageSize, &imageWidth, &imageHeight, &comp, 4);

        /* Récupération du tableau correspondant à l'image. */
        if (pthread_mutex_trylock(context->imageMutex) == 0) {
            struct hashMap *tempHashMap = context->previousHashMap;
            context->previousHashMap = context->hashMap;
            context->hashMap = tempHashMap;
            
            /* Nettoyage de l'ancienne hash map. */
            clearHashMap(context->hashMap);

            if (context->pixelArray != NULL) stbi_image_free(context->pixelArray);
            context->pixelArray = pixelArray;

            if (context->detectionList != NULL) free(context->detectionList);
            context->detectionList = detectionList;

            /* Création des noeuds pour chaque détection reçue. */
            for (int i = 0; i < context->numBoxes; i++) {
                createNode(context->detectionList[i], coordinateList[4 * i], coordinateList[4 * i + 1], coordinateList[4 * i + 2], coordinateList[4 * i + 3], context->hashMap);
            }

            /* Gestion de l'offset via l'objet de référence "tv". */
            if (nodeExists("tv", context->hashMap) && epipolarCalibration == 0) {
                context->offsetX = getX("tv", context->hashMap);
                context->offsetY = getY("tv", context->hashMap);
                for (int i = 0; i < context->numBoxes; i++) {
                    updateNode(context->detectionList[i], coordinateList[4 * i], coordinateList[4 * i + 1], coordinateList[4 * i] - context->offsetX, coordinateList[4 * i + 1] - context->offsetY, coordinateList[4 * i + 2], coordinateList[4 * i + 3], context->hashMap);
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


/** Initialise le contexte propre à un thread de réception.
 * @param context Le pointeur vers la structure du contexte à initialiser
 * @param texture La texture SDL utilisée pour le rendu
 * @param renderer Le renderer SDL associé
 * @param index L'index du thread
 * @param workerIndex L'index de la worker assignée
 * @param mutex Le mutex pour l'image
 * @param firstMutex Le mutex pour la première image
 * @param firstCondition La condition pour la première image
 */
void initializeContext(struct threadContext *context, SDL_Texture *texture, SDL_Renderer *renderer, int index, int workerIndex, pthread_mutex_t *mutex, pthread_mutex_t *firstMutex, pthread_cond_t *firstCondition) {
    context->numBoxes = 0;
    context->canGetImage = 0;
    context->index = index;
    context->firstImageReady = 0;
    context->pixelArray = NULL;
    context->texture = texture;
    context->imageMutex = mutex;
    context->firstImageMutex = firstMutex;
    context->firstImageCondition = firstCondition;
    context->renderer = renderer;
    context->detectionList = NULL;
    context->workerIndex = workerIndex;
    context->hashMap = initializeHashMap(32);
    context->previousHashMap = initializeHashMap(32);
    context->offsetX = 0;
    context->offsetY = 0;
    context->imageWidth = 0;
}


/** Affiche le texte `text` à l'écran aux positions `x` et `y`.
 * @param renderer Le renderer SDL sur lequel afficher le texte
 * @param font La police d'écriture à utiliser
 * @param text La chaîne de caractères à afficher
 * @param x La coordonnée x du coin supérieur gauche du texte
 * @param y La coordonnée y du coin supérieur gauche du texte
 * @param color La couleur du texte
 */
void renderText(SDL_Renderer *renderer, TTF_Font *font, const char *text, int x, int y, SDL_Color color) {
    if (font == NULL || text == NULL) return;

    /* Création d'une surface. */
    SDL_Surface *surface = TTF_RenderText_Solid(font, text, color);
    if (surface == NULL) return;

    /* Conversion en texture. */
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    
    SDL_Rect destRect = {x, y, surface->w, surface->h};
    
    /* Copie sur le renderer. */
    SDL_RenderCopy(renderer, texture, NULL, &destRect);

    SDL_FreeSurface(surface);
    SDL_DestroyTexture(texture);
}


/** Affiche l'image et ses boîtes de détection sur la fenêtre SDL du thread.
 * @param context Le contexte du thread contenant les informations à afficher
 */
void renderImage(struct threadContext *context) {
    pthread_mutex_lock(context->imageMutex);

    /* Si aucune image n'est prête à être affichée, on libère le mutex et on quitte. */
    if (context->canGetImage != 1 || context->pixelArray == NULL) {
        pthread_mutex_unlock(context->imageMutex);
        return;
    }

    /* Mise à jour de la texture avec le nouveau tableau de pixels et affichage. */
    SDL_UpdateTexture(context->texture, NULL, context->pixelArray, context->imageWidth * 4);
    SDL_RenderClear(context->renderer);
    SDL_RenderCopy(context->renderer, context->texture, NULL, NULL);

    /* Réinitialisation du flag de disponibilité de l'image. */
    context->canGetImage = 0;

    /* Parcours de la hash map pour dessiner les boîtes de détection. */
    for (int i = 0; i < context->hashMap->capacity; i++) {
        struct node *toExplore = context->hashMap->hashTable[i];

        while (toExplore != NULL) {
            int x1 = toExplore->x;
            int y1 = toExplore->y;
            
            /* Récupération des dimensions de la boîte de détection. */
            int boxWidth = toExplore->width; 
            int boxHeight = toExplore->height;
            
            /* Définition de la couleur de dessin pour la boîte (vert). */
            SDL_SetRenderDrawColor(context->renderer, 0, 255, 0, 255);

            /* Création du rectangle SDL pour la caméra 1 */
            SDL_Rect rectangle;
            rectangle.w = boxWidth;
            rectangle.h = boxHeight;
            rectangle.x = (x1 + context->offsetX);
            rectangle.y = (y1 + context->offsetY);

            SDL_RenderDrawRect(context->renderer, &rectangle);
            SDL_Color textColor = (SDL_Color){0, 255, 0, 255};
            renderText(context->renderer, font, toExplore->key, rectangle.x, rectangle.y - 20, textColor);
            
            toExplore = toExplore->next;
        }
    }

    pthread_mutex_unlock(context->imageMutex);

    SDL_RenderPresent(context->renderer);
}


/** Affiche la vue globale et reconstruit la scène en fonction des détections des deux caméras.
 * @param firstContext Le contexte du premier thread contenant sa hash map
 * @param secondContext Le contexte du deuxième thread contenant sa hash map
 */
void renderScene(struct threadContext *firstContext, struct threadContext *secondContext) {
    pthread_mutex_lock(firstContext->imageMutex);
    pthread_mutex_lock(secondContext->imageMutex);

    /* Réinitialisation de l'écran en noir. */
    SDL_SetRenderDrawColor(firstContext->renderer, 0, 0, 0, 255);
    SDL_RenderClear(firstContext->renderer);

    /* On s'assure qu'on a bien des données à traiter */
    if (firstContext->detectionList == NULL || secondContext->detectionList == NULL) {
        goto unlock_mutexes_and_present;
    }

    /* Parcours des détections de la première caméra */
    for (int i = 0; i < firstContext->hashMap->capacity; i++) {
        struct node *toExplore = firstContext->hashMap->hashTable[i];

        while (toExplore != NULL) {
            int x1 = toExplore->x;
            int y1 = toExplore->y;
            
            int boxWidth = toExplore->width; 
            int boxHeight = toExplore->height;

            if (x1 == ERROR_COORDINATE || y1 == ERROR_COORDINATE) {
                toExplore = toExplore->next;
                continue;
            }

            int matchFound = 0;

            struct node *camNode = secondContext->hashMap->hashTable[getHashValue(toExplore->key, secondContext->hashMap)];
            while (camNode != NULL) {
                if (strcmp(camNode->key, toExplore->key) != 0 || camNode->isSeen == 1) {
                    camNode = camNode->next;
                    continue;
                }

                /* Calcul de la différence absolue des coordonnées entre les deux caméras. */
                float deltaX = ((float) x1 - camNode->x);
                float deltaY = ((float) y1 - camNode->y);
                
                deltaX = deltaX > 0 ? deltaX : -deltaX;
                deltaY = deltaY > 0 ? deltaY : -deltaY;

                /* Si la distance entre les objets est inférieure au seuil, on considère que c'est le même objet. */
                if (deltaX <= 0.15 * 640 && deltaY <= 0.15 * 640) {
                    matchFound = 1;
                    camNode->isSeen = 1;
                    break;
                }
                
                camNode = camNode->next;
            }

            /* Choix de la couleur : vert si mutualisée, bleu sinon */
            if (matchFound) {
                SDL_SetRenderDrawColor(firstContext->renderer, 0, 255, 0, 255);
            } else {
                SDL_SetRenderDrawColor(firstContext->renderer, 0, 0, 255, 255);
            }

            /* Création du rectangle SDL pour la caméra 1 */
            SDL_Rect rectangle;
            rectangle.w = boxWidth;
            rectangle.h = boxHeight;

            /* Translation vers le repère SDL : on décale de (width / 2, height / 2) vers le centre. */
            rectangle.x = (x1 + 640) - (boxWidth / 2);
            rectangle.y = (y1 + 450) - (boxHeight / 2);

            SDL_RenderDrawRect(firstContext->renderer, &rectangle);
            SDL_Color textColor = matchFound ? (SDL_Color){0, 255, 0, 255} : (SDL_Color){0, 0, 255, 255};
            renderText(firstContext->renderer, font, toExplore->key, rectangle.x, rectangle.y - 20, textColor);
            
            toExplore = toExplore->next;
        }
    }

    /* Affichage des objets détectés uniquement par la caméra 2 (non mutualisés) */
    SDL_SetRenderDrawColor(secondContext->renderer, 0, 0, 255, 255);

    for (int i = 0; i < secondContext->hashMap->capacity; i++) {
        struct node *toExplore = secondContext->hashMap->hashTable[i];

        while (toExplore != NULL) {
            if (toExplore->isSeen == 0) {
                SDL_Rect rectangle;
                rectangle.w = toExplore->width;
                rectangle.h = toExplore->height;
                rectangle.x = (toExplore->x + 640) - (rectangle.w / 2);
                rectangle.y = (toExplore->y + 450) - (rectangle.h / 2);

                SDL_RenderDrawRect(secondContext->renderer, &rectangle);
                renderText(firstContext->renderer, font, toExplore->key, rectangle.x, rectangle.y - 20, (SDL_Color){0, 0, 255, 255});
            }

            toExplore = toExplore->next;
        }
    }

    firstContext->canGetImage = 0;
    secondContext->canGetImage = 0;

unlock_mutexes_and_present:
    pthread_mutex_unlock(firstContext->imageMutex);
    pthread_mutex_unlock(secondContext->imageMutex);

    /* On affiche le résultat graphique final */
    SDL_RenderPresent(firstContext->renderer);
}


/** Affiche dans la console les objets détectés par les deux caméras et mutualise les détections communes.
 * @param context1 Le contexte du premier thread
 * @param context2 Le contexte du deuxième thread
 */
void printDetections(struct threadContext *context1, struct threadContext *context2) {
    pthread_mutex_lock(context1->imageMutex);
    pthread_mutex_lock(context2->imageMutex);

    /* Si les hash maps n'ont pas changé depuis la dernière itération, on ne réaffiche pas. */
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

            int matchFound = 0;

            /* Initialisation des variables pour la droite épipolaire. */
            long double a = 1;
            long double b = 1;
            long double c = 1;
            long double norm = 1;

            if (epipolarCalibration == 1) {
                a = F[0][0] * x1 + F[0][1] * y1 + F[0][2];
                b = F[1][0] * x1 + F[1][1] * y1 + F[1][2];
                c = F[2][0] * x1 + F[2][1] * y1 + F[2][2];
                norm = sqrtf(a * a + b * b);
            }

            struct node *camNode = context2->hashMap->hashTable[getHashValue(toExplore->key, context2->hashMap)];
            while (camNode != NULL) {
                if (strcmp(camNode->key, toExplore->key) != 0 || camNode->isSeen == 1) {
                    camNode = camNode->next;
                    continue;
                }

                if (epipolarCalibration == 1) {
                    /* Calcul de la distance du point à la droite épipolaire correspondante. */
                    long double distance = fabsl(a * camNode->x + b * camNode->y + c) / norm;

                    if (distance <= 40.0) {
                        matchFound = 1;
                        camNode->isSeen = 1;
                        break;
                    }
                } else {
                    /* Si la calibration n'est pas active, on utilise une distance euclidienne classique. */
                    float deltaX = 640;
                    float deltaY = 640;

                    deltaX = ((float) x1 - camNode->x);
                    deltaY = ((float) y1 - camNode->y);

                    deltaX = deltaX > 0 ? deltaX : -deltaX;
                    deltaY = deltaY > 0 ? deltaY : -deltaY;

                    if (deltaX <= 0.15 * 640 && deltaY <= 0.15 * 640) {
                        matchFound = 1;
                        camNode->isSeen = 1;
                        break;
                    }
                }
                
                camNode = camNode->next;

            }

            if (matchFound) {
                printf("Détection mutualisée de %s à (%d, %d).\n", toExplore->key, x1, y1);
            } else {
                printf("Détection par orin-nano-%d de %s à (%d, %d).\n", context1->workerIndex, toExplore->key, x1, y1);
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


/** Point d'entrée du programme initialisant les composants SDL, les threads et la communication réseau.
 * @param argc Le nombre d'arguments en ligne de commande
 * @param argv Le tableau contenant les arguments
 * @return EXIT_SUCCESS lors de la fin de l'exécution
 */
int main(int argc, char *argv[]) {
    init_connection();

    /* Vérification de l'input utilisateur. */
    if (argc == 4) {
        for (int i = 0; i < argc; i++) {
            if (strcmp(argv[i], "--epipolar") == 0) {
                epipolarCalibration = 1;
                break;
            }
            if (strcmp(argv[i], "--reconstruct") == 0) {
                sceneReconstruction = 1;
                break;
            }
        }
    } else if (argc != 3) {
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

    if (TTF_Init() == -1) {
        printf("ERREUR : L'affichage de texte à l'écran n'a pas pu être initialisé.\n");
        exit(EXIT_FAILURE);
    }

    font = TTF_OpenFont("arial.ttf", 16); 
    if (font == NULL) {
        printf("ERREUR : Impossible de charger la police : %s\n", TTF_GetError());
        exit(EXIT_FAILURE);
    }
    
    /* Récupération de la taille de l'écran. */
    SDL_Rect screenRect;
    SDL_GetDisplayBounds(0, &screenRect);

    /* Initialisation des pointeurs vers les fenêtres SDL. */
    SDL_Window *window;
    SDL_Window *secondWindow;

    if (sceneReconstruction == 1) {
        window = SDL_CreateWindow("Flux reconstruit", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 900, 0);
        secondWindow = window;
    } else {
        window = SDL_CreateWindow("Flux 1 Jetson Orin Nano", screenRect.w / 2 - 640, SDL_WINDOWPOS_CENTERED, 640, 640, 0);
        secondWindow = SDL_CreateWindow("Flux 2 Jetson Orin Nano", screenRect.w / 2, SDL_WINDOWPOS_CENTERED, 640, 640, 0);
    }

    if (window == NULL || secondWindow == NULL) {
        printf("ERREUR : La fenêtre d'affichage n'a pas pu être créé.\n");
        exit(EXIT_FAILURE);
    }

    /* Initialisation des pointeurs vers les renderers SDL. */
    SDL_Renderer *renderer;
    SDL_Renderer *secondRenderer;

    if (sceneReconstruction == 1) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_TARGETTEXTURE);
        secondRenderer = renderer;
    } else {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_TARGETTEXTURE);
        secondRenderer = SDL_CreateRenderer(secondWindow, -1, SDL_RENDERER_TARGETTEXTURE);
    }

    if (renderer == NULL || secondRenderer == NULL) {
        printf("ERREUR : Le renderer de la fenêtre n'a pas pu être créé.\n");
        exit(EXIT_FAILURE);
    }

    /* Initialisation des pointeurs vers les textures SDL. */
    SDL_Texture *texture;
    SDL_Texture *secondTexture;

    if (sceneReconstruction == 1) {
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 1280, 900);
        secondTexture = texture;
    } else {
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 640, 640);
        secondTexture = SDL_CreateTexture(secondRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 640, 640);
    }

    if (texture == NULL || secondTexture == NULL) {
        printf("ERREUR : La texture n'a pas pu être créée.\n");
        exit(EXIT_FAILURE);
    }

    /* Initialisation des mutexes et des variables conditions globales. */
    pthread_mutex_t firstThreadMutex;
    pthread_mutex_t secondThreadMutex;

    pthread_mutex_init(&firstThreadMutex, NULL);
    pthread_mutex_init(&secondThreadMutex, NULL);
    pthread_mutex_init(&updatedMutex, NULL);
    pthread_mutex_init(&imageMutex1, NULL);
    pthread_mutex_init(&imageMutex2, NULL);
    pthread_cond_init(&imageCondition1, NULL);
    pthread_cond_init(&imageCondition2, NULL);

    /* Récupération d'un ID d'event SDL valide. */
    eventID = SDL_RegisterEvents(1);

    /* Création du premier thread d'écoute. */
    struct threadContext firstThreadContext;
    initializeContext(&firstThreadContext, texture, renderer, 0, workerIndex1, &firstThreadMutex, &imageMutex1, &imageCondition1);

    pthread_t firstReceivingThread;
    if (pthread_create(&firstReceivingThread, NULL, receivingThreadMain, &firstThreadContext) != 0) {
        printf("ERREUR : Le premier thread d'écoute du flux vidéo n'a pas pu être créé.\n");
        exit(EXIT_FAILURE);
    } else {
        pthread_detach(firstReceivingThread);
    }

    /* Création du second thread d'écoute. */
    struct threadContext secondThreadContext;
    initializeContext(&secondThreadContext, secondTexture, secondRenderer, 1, workerIndex2, &secondThreadMutex, &imageMutex2, &imageCondition2);

    pthread_t secondReceivingThread;
    if (pthread_create(&secondReceivingThread, NULL, receivingThreadMain, &secondThreadContext) != 0) {
        printf("ERREUR : Le deuxième thread d'écoute du flux vidéo n'a pas pu être créé.\n");
        exit(EXIT_FAILURE);
    } else {
        pthread_detach(secondReceivingThread);
    }

    
    if (epipolarCalibration == 1) {
        /* Boucles d'attente des signaux des 2 images initiales. */
        pthread_mutex_lock(&imageMutex1);
        while(firstThreadContext.firstImageReady != 1) {
            pthread_cond_wait(&imageCondition1, &imageMutex1);
        }
        pthread_mutex_unlock(&imageMutex1);

        pthread_mutex_lock(&imageMutex2);
        while(secondThreadContext.firstImageReady != 1) {
            pthread_cond_wait(&imageCondition2, &imageMutex2);
        }
        pthread_mutex_unlock(&imageMutex2);

        /* Structure Windows nécessaire pour créer un processus. */
        STARTUPINFO si;
        PROCESS_INFORMATION pi;

        /* Initialisation des structures. */
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

        char cmdLine[] = "python epipolar.py"; 

        /* Création du processus fils (Windows). */
        BOOL success = CreateProcess(
            NULL,           // Application name
            cmdLine,        // Ligne de commande complète
            NULL,           // Sécurité du processus
            NULL,           // Sécurité du thread
            FALSE,          // Héritage des handles
            0,              // Drapeaux de création
            NULL,           // Environnement
            NULL,           // Répertoire de travail actuel
            &si,            // Pointeur vers STARTUPINFO
            &pi             // Pointeur vers PROCESS_INFORMATION
        );

        if (!success) {
            printf("ERREUR : Le processus fils n'a pas pu être créé. Code d'erreur : %lu\n", GetLastError());
            epipolarCalibration = 0;
        } else {
            /* Dans le père, on attend la fin de l'exécution du fils (Windows). */
            WaitForSingleObject(pi.hProcess, INFINITE);

            DWORD exitCode;
            if (!GetExitCodeProcess(pi.hProcess, &exitCode) || exitCode != 0) {
                printf("ERREUR : Le lancement s'est mal déroulé.\n");
                epipolarCalibration = 0;
            } else {
                printf("Calibration terminée avec succès, récupération de la matrice fondamentale.\n");

                int resultsFd;
                if ((resultsFd = open("./calibration/results.txt", O_RDONLY)) == -1) {
                    printf("ERREUR : Impossible d'ouvrir le fichier contenant la matrice fondamentale.\n");
                    epipolarCalibration = 0;
                    close(resultsFd);
                    goto end_epipolar;
                }

                /* Récupération de la matrice fondamentale depuis le fichier results.txt. */
                FILE *resultFile = fdopen(resultsFd, "r");
                for (int i = 0; i < 3; i++) {
                    for (int j = 0; j < 3; j++) {
                        long double coefficient;
                        fscanf(resultFile, "%Lf\n", &coefficient);
                        F[i][j] = coefficient;
                    }
                }
                end_epipolar:
                fclose(resultFile);
            }

            /* Fermeture des handles ouverts. */
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }

    /* Boucle principale pour la gestion des évènements SDL. */
    SDL_Event event;
    while (1) {
        
        /* Afficher l'image à l'écran avec SDL. */
        while (SDL_WaitEvent(&event)) {
            if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) exit(EXIT_SUCCESS);

            int shouldPrint = 0;
            pthread_mutex_lock(&updatedMutex);
            if (hashMapUpdated == (0 | 1 << workerIndex1 | 1 << workerIndex2)) {
                shouldPrint = 1;
                hashMapUpdated = 0;
            }
            pthread_mutex_unlock(&updatedMutex);

            /* Affichage des détections si les hash maps ont été mises à jour. */
            if (shouldPrint && sceneReconstruction == 0) {
                printDetections(&firstThreadContext, &secondThreadContext);
                shouldPrint = 0;
            }

            if (event.type != eventID) continue;

            if (sceneReconstruction == 1) {
                renderScene(&firstThreadContext, &secondThreadContext);
                continue;
            }

            if (event.user.code == (Sint32) workerIndex1) renderImage(&firstThreadContext);

            if (event.user.code == (Sint32) workerIndex2) renderImage(&secondThreadContext);
        }
    }

    close_connection();
}