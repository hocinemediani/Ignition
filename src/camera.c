#include "camera.h"

/* Descripteur de fichier pointant vers le flux de la caméra. */
int desiredCameraFd = -1;
/* Tableau de structures stockant les adresses et tailles des buffers vidéo. */
struct videoBuffer videoBuffers[NUM_BUFFERS];
/* Mutexes pour la protection de la lecture / écriture dans les buffers. */
pthread_mutex_t bufferMutexes[NUM_BUFFERS];
/* Structure sous contenant les informations sur le dernier buffer rempli. */
struct pythonMessage message;
/* Mutex pour l'édition du message à envoyer. */
pthread_mutex_t messageMutex;
/* Struture contenant la résolution effective de la caméra. */
struct cameraInfo camInfo;
/* Table des sockets clients connectés. */
int socketTable[MAX_CONCURRENT_CONNECTIONS];
/* Nombre de clients actuellement connectés. */
int connectedClients = 0;

/** Envoie le message débutant à l'adresse mémoire `messageToSend`, sur le socket `clientSocket`
 * et de taille `size`.
 * @param clientSocket Le socket sur lequel envoyer le message
 * @param messageToSend Un pointeur vers le message à envoyer
 * @param size La taille du message à envoyer
 * @return 0 si l'envoi s'est bien déroulé, -1 sinon
*/
int sendMessage(int clientSocket, const void *messageToSend, int size) {
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


/** Ferme le programme avec le code d'erreur `exitCode` et désalloue les
 * `toClean` buffers alloués avant l'appel à `endProgram`.
 * @param toClean Le nombre de buffers vidéo alloués
 * @param exitCode Le code de sortie pour la terminaison du programme
*/
void endProgram(int toClean, int exitCode) {
    for (int i = 0; i < toClean; i++) {
        munmap(videoBuffers[i].start, videoBuffers[i].length);
    }
    exit(exitCode);
}


/** Renvoie une structure `cameraInfo` contenant la résolution de la caméra.
 * Appelé depuis le script python main.py afin de connaître la résolution vidéo
 * pour les opérations de scaling, letter-boxing etc.
 * @return La structure contenant les informations sur la caméra
 */
struct cameraInfo getCameraInfo() {
    return camInfo;
}


/** Permet de vérifier l'existence ou non du fichier .engine nécessaire à l'inférence
 * et build ce dernier si il ne l'est pas (l'opération prends plusieurs dizaines de minutes).
 * @param modelPath Le nom du fichier du modèle
 * @param needsRebuild Un flag permettant de forcer le rebuild de l'engine
 */
void loadEngine(const char *modelPath, int needsRebuild) {
    int engineFd = open("./models/yolov8n.engine", O_RDONLY);
    if (needsRebuild == 0 && engineFd != -1) {
        printf("Engine trouvé, étape de build ignorée.\n");
        close(engineFd);
        return;
    }
    printf("Engine non trouvé, lancement du build.\n");

    /* Préparation de la commande pour build l'engine. */
    char *argList[5];
    argList[0] = strdup("/usr/src/tensorrt/bin/trtexec");
    argList[1] = malloc(7 + strlen(modelPath) + 1);
    sprintf(argList[1], "--onnx=%s", modelPath);
    argList[2] = strdup("--saveEngine=./models/yolov8n.engine");
    argList[3] = strdup("--fp16");
    argList[4] = NULL;

    pid_t pid = fork();

    /* Création d'un processus fils pour build l'engine. */
    if (pid == -1) {
        printf("ERREUR : Impossible de créer le processus fils de build de l'engine.\n");
        endProgram(0, EXIT_FAILURE);
    } else if (pid == 0) {
        /* Dans le fils, on renvoie la sortie standard vers /dev/null pour éviter
        * les logs de build et on lance l'opération. */
        int nullFd = open("/dev/null", O_WRONLY);
        dup2(nullFd, STDOUT_FILENO);
        close(nullFd);
        execvp("/usr/src/tensorrt/bin/trtexec", argList);
        endProgram(0, EXIT_FAILURE);
    } else {
        /* Dans le père, on attends la terminaison du fils. */
        int status;
        wait(&status);
        if (WEXITSTATUS(status) == 0) {
            printf("Build du fichier .engine terminé.\n");
        } else {
            endProgram(0, EXIT_FAILURE);
        }
    }
}


/** Fonction helper permettant de filtrer les noms de fichier dans /dev/ pour
 * ne garder que les entrées vidéos.
 * @param file Un pointeur vers la structure contenant les informations utiles sur le fichier
 * @return 1 si le nom du fichier contient 'video', 0 sinon
*/
int filterFileName(const struct dirent *file) {
    if (strncmp(file->d_name, "video", 5) == 0) return 1;
    return 0;
}


/** Permet de re-empiler le buffer d'index `index` après la récupération
 * de l'image contenue à l'intérieur et la libération du mutex associé.
 * @param index L'index du buffer à libérer
 */
void releaseLastFrame(int index) {
    /* On recrée une structure de buffer v4l2 pour ensuite la re-empiler. */
    struct v4l2_buffer buffer;
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;

    /* Important, l'index doit correspondre à celui du buffer qui à été pris lors de
    * l'appel à getLastFrame. */
    buffer.index = index;
    ioctl(desiredCameraFd, VIDIOC_QBUF, &buffer);
    pthread_mutex_unlock(&bufferMutexes[index]);
}


/** Renvoie une structure `pythonMessage` contenant un pointeur vers le début
 * du buffer vidéo, la taille utilisée et l'index du buffer vidéo.
 * @return La structure `pythonMessage` contenant les informations utiles sur la dernière frame récupérée
 */
struct pythonMessage getLastFrame() {
    /* Tentative de récupération du buffer ayant la dernière image. */
    pthread_mutex_lock(&messageMutex);
    
    /* On marque ce buffer comme utilisé. */
    pthread_mutex_lock(&bufferMutexes[message.bufferIndex]);

    /* Copie du message à envoyer étant donné qu'on ne pourra pas unlock après le return. */
    struct pythonMessage messageToSend = message;
    pthread_mutex_unlock(&messageMutex);

    return messageToSend;
}


/** Point d'entrée pour le thread de récupération des frames depuis la caméra.
 * @param _arg (non utilisé)
 * @return NULL lors de la terminaison du thread
 */
void* threadMain(void *_arg) {
    (void) _arg;

    int previousBufferIndex = -1;
    while (1) {
        /* Récupération du dernier buffer rempli par la caméra. */
        struct v4l2_buffer buffer;
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        ioctl(desiredCameraFd, VIDIOC_DQBUF, &buffer);
        
        /* Préparation du message pour le script python. */
        pthread_mutex_lock(&messageMutex);
        message.bufferIndex = buffer.index;
        message.start = videoBuffers[buffer.index].start;
        message.size = buffer.bytesused;
        pthread_mutex_unlock(&messageMutex);

        /* Si on viens de commencer à récupérer les images. */
        if (previousBufferIndex == -1) goto skip;

        /* Si le buffer n'est pas utilisé par le script python, il est rendu. */
        if (pthread_mutex_trylock(&bufferMutexes[previousBufferIndex]) == 0) {
            struct v4l2_buffer bufferToGiveback;
            bufferToGiveback.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            bufferToGiveback.memory = V4L2_MEMORY_MMAP;
            bufferToGiveback.index = previousBufferIndex;
            ioctl(desiredCameraFd, VIDIOC_QBUF, &bufferToGiveback);
            pthread_mutex_unlock(&bufferMutexes[previousBufferIndex]);
        }

        skip:
        previousBufferIndex = buffer.index;
    }

    endProgram(NUM_BUFFERS, EXIT_SUCCESS);
    return NULL;
}


/** Point d'entrée pour le thread de gestion des connexions simultannées.
 * @param _arg (non utilisé)
 * @return NULL lors de la terminaison du thread
 */
void* receivingMain(void *_arg) {
    (void) _arg;

    /* Création du socket de communication de la worker.*/
    int workerSocket;
    if ((workerSocket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        printf("ERREUR : Le socket du worker n'a pas pu être créé.\n");
        endProgram(NUM_BUFFERS, EXIT_FAILURE);
    }

    /* Changement du flag `SO_REUSEADDR` pour permettre de relancer le programme et de libérer le port
    * utilisé instantannément. */
    setsockopt(workerSocket, SOL_SOCKET, SO_REUSEADDR, NULL, 0);

    /* Ecoute sur le port 9988 de la worker. */
    struct sockaddr_in workerAddress;
    memset(&workerAddress, 0, sizeof(workerAddress));
    workerAddress.sin_addr.s_addr = INADDR_ANY;
    workerAddress.sin_family = AF_INET;
    workerAddress.sin_port = htons(9988);

    if (bind(workerSocket, (const struct sockaddr *) &workerAddress, sizeof(workerAddress)) == -1) {
        printf("ERREUR : Le bind du socket à échoué.\n");
        endProgram(NUM_BUFFERS, EXIT_FAILURE);
    }

    if (listen(workerSocket, MAX_CONCURRENT_CONNECTIONS) == -1) {
        printf("ERREUR : Le listen sur le socket n'a pas pu aboutir.\n");
        endProgram(NUM_BUFFERS, EXIT_FAILURE);
    }

    fd_set socketReadSet;

    /* Initialisation de la table des sockets clients. */
    for (int i = 0; i < MAX_CONCURRENT_CONNECTIONS; i++) {
        socketTable[i] = 0;
    }

    while (1) {
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 500;

        /* Remise à zéro du set et insertion du socket sur lequel on écoute (le socket worker). */
        FD_ZERO(&socketReadSet);
        FD_SET(workerSocket, &socketReadSet);

        int maxSocketFd = workerSocket;

        /* Ajout des sockets clients dans le set afin de gérer les déconnexions proprement. */
        for (int i = 0; i < connectedClients; i++) {
            FD_SET(socketTable[i], &socketReadSet);
            if (socketTable[i] > maxSocketFd) maxSocketFd = socketTable[i];
        }

        /* Attente de la mise à jour d'un des descripteurs de fichier. */
        int selectReturn = select(maxSocketFd + 1, &socketReadSet, NULL, NULL, &timeout);

        if (selectReturn == -1) continue;

        /* Lorsque le socket de la worker à été mis à jour, une nouvelle connexion s'est produite. */
        if (FD_ISSET(workerSocket, &socketReadSet) && (connectedClients < MAX_CONCURRENT_CONNECTIONS)) {
            socketTable[connectedClients++] = accept(workerSocket, NULL, NULL);
            printf("Une nouvelle connexion s'est produite !\n");
        }

        /* Si un des sockets client à été mis à jour, alors une déconnexion s'est produite. */
        for (int i = 0; i < connectedClients; i++) {
            if (FD_ISSET(socketTable[i], &socketReadSet)) {
                close(socketTable[i]);
                socketTable[i] = socketTable[connectedClients - 1];
                connectedClients--;
                i--;
            }
        }
    }
}


/** Envoie à tous les clients connectés le message `data` de taille `size`.
 * @param data La message à envoyer
 * @param size La taille du message à envoyer
*/
void sendData(void *data, uint32_t size) {
    int networkSize = htonl(size);
    for (int i = 0; i < connectedClients; i++) {
        /* Envoi de la taille pour que la réception se fasse correctement. */
        sendMessage(socketTable[i], &networkSize, sizeof(networkSize));
        sendMessage(socketTable[i], data, size);
    }
}


/** Initialise la caméra en itérant à travers tous les périphériques de capture vidéo disponible,
 * puis en initialisant les buffers de récupération du flux et en lancant les threads de récupération
 * de l'image et de gestion des connexions utilisateur.
 * @param cameraWidth La largeur voulue pour le flux vidéo
 * @param cameraHeight La hauteur voulue pour le flux vidéo
 * @param cameraIndex L'index voulu pour la caméra (la première est à l'index 0, etc)
*/
void initializeCamera(int cameraWidth, int cameraHeight, int cameraIndex) {
    memset(&camInfo, 0, sizeof(camInfo));

    /* Récupération des différentes caméras disponibles. */
    struct dirent **namelist;
    int scanReturn = scandir("/dev/", &namelist, filterFileName, alphasort);
    if (scanReturn <= 0) {
        printf("ERREUR : Il y a eu un problème lors de la récupération des caméras.\n");
        exit(EXIT_FAILURE);
    }

    /* Affichage des caméras disponibles. */
    char filePath[266];
    int numCameras = 0;
    printf("Ensemble de caméras disponibles :\n");
    for (int i = 0; i < scanReturn; i++) {
        snprintf(filePath, sizeof(filePath), "/dev/%s", namelist[i]->d_name);

        int cameraFd = open(filePath, O_RDWR);
        if (cameraFd == -1) {
            printf("ERREUR : Impossible d'ouvrir le périphérique %s.\n", namelist[i]->d_name);
            continue;
        }

        /* Récupération des informations utiles sur la caméra. */
        struct v4l2_capability capability;
        if (ioctl(cameraFd, VIDIOC_QUERYCAP, &capability) == -1) {
            printf("ERREUR : Impossible de récupérer les informations de l'équipement %s.\n", namelist[i]->d_name);
            continue;
        }

        /* Si il s'agit bien d'une caméra, on l'affiche. */
        if (capability.device_caps & V4L2_CAP_VIDEO_CAPTURE) {
            printf("\t %d. - %s\n", i, capability.card);
            numCameras++;
        }

        if (i == cameraIndex) {
            desiredCameraFd = cameraFd;
            continue;
        }
        close(cameraFd);
    }

    /* Après avoir affiché les caméras disponibles, on vérifie que l'utilisateur en ait choisit une d'index correct. */
    if (cameraIndex >= numCameras) {
        printf("ERREUR : Il n'y a pas de caméra d'index %d.\n", cameraIndex);
        exit(EXIT_FAILURE);
    }

    /* A présent, on affiche les résolutions disponibles pour la caméra. */
    struct v4l2_frmsizeenum frmsize = {0};
    frmsize.index = 0;
    frmsize.pixel_format = V4L2_PIX_FMT_YUYV;

    printf("Résolutions disponibles :\n");
    while (ioctl(desiredCameraFd, VIDIOC_ENUM_FRAMESIZES, &frmsize) != -1) {
        if (frmsize.type != V4L2_FRMSIZE_TYPE_DISCRETE) continue;
        printf("\t - %d:%d\n", frmsize.discrete.width, frmsize.discrete.height);
        frmsize.index++;
    }

    /* Puis on donne le format souhaité à la caméra. */
    struct v4l2_format videoFormat = {0};
    videoFormat.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    videoFormat.fmt.pix.width = cameraWidth;
    videoFormat.fmt.pix.height = cameraHeight;
    videoFormat.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    videoFormat.fmt.pix.field = V4L2_FIELD_ANY;

    if (ioctl(desiredCameraFd, VIDIOC_S_FMT, &videoFormat) == -1) {
        printf("ERREUR : Impossible de configurer la caméra avec la résolution souhaitée.\n");
        exit(EXIT_FAILURE);
    }

    camInfo.width = videoFormat.fmt.pix.width;
    camInfo.height = videoFormat.fmt.pix.height;
    printf("Format utilisé par la caméra : %d:%d\n", videoFormat.fmt.pix.width, videoFormat.fmt.pix.height);

    /* Configuration des paramètres du flux vidéo, on utilise des buffers memory-mapped
    * afin que la caméra ait un DMA sur le flux et que la récupération de l'image se fasse
    * en zero-copy pour éviter l'overhead. */
    struct v4l2_requestbuffers reqBuffers = {0};
    reqBuffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqBuffers.memory = V4L2_MEMORY_MMAP;
    reqBuffers.count = NUM_BUFFERS;
    if (ioctl(desiredCameraFd, VIDIOC_REQBUFS, &reqBuffers) == -1) {
        printf("ERREUR : Impossible d'allouer les buffers vidéo.\n");
        exit(EXIT_FAILURE);
    }

    /* Récupération et stockage des addresses de la mémoire partagée avec la caméra. */
    for (int i = 0; i < NUM_BUFFERS; i++) {
        struct v4l2_buffer videoBuffer;
        videoBuffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        videoBuffer.memory = V4L2_MEMORY_MMAP;
        videoBuffer.index = i;

        if (ioctl(desiredCameraFd, VIDIOC_QUERYBUF, &videoBuffer) == -1) {
            printf("ERREUR : Le buffer vidéo %d n'a pas pu être configuré.\n", i);
            endProgram(i, EXIT_FAILURE);
        }

        /* Après récupération du buffer vidéo, on le sauvegarde dans notre table de buffers. */
        videoBuffers[i].length = videoBuffer.length;
        videoBuffers[i].start = mmap(NULL, videoBuffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, desiredCameraFd, videoBuffer.m.offset);

        if (videoBuffers[i].start == MAP_FAILED) {
            printf("ERREUR : Impossible de récupérer l'adresse de la mémoire partagée pour le buffer %d.\n", i);
            endProgram(i, EXIT_FAILURE);
        }

        /* On empile les buffers. */
        ioctl(desiredCameraFd, VIDIOC_QBUF, &videoBuffer);
    }

    /* Initilisation des mutexes pour les buffers. */
    pthread_mutex_init(&messageMutex, NULL);
    for (int i = 0; i < NUM_BUFFERS; i++) {
        pthread_mutex_init(&bufferMutexes[i], NULL);
    }

    /* Lancement du flux vidéo. */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(desiredCameraFd, VIDIOC_STREAMON, &type) == -1) {
        printf("ERREUR : Le lancement de la caméra à échoué.\n");
        endProgram(NUM_BUFFERS, EXIT_FAILURE);
    }
    
    /* Lancement du thread récupérant le dernier pointeur vers un tableau de pixels disponible. */
    pthread_t imageYieldingThread;
    if (pthread_create(&imageYieldingThread, NULL, threadMain, NULL) != 0) {
        printf("ERREUR : Le thread de récupération des images n'a pas pu être créé.\n");
        endProgram(NUM_BUFFERS, EXIT_FAILURE);
    } else {
        pthread_detach(imageYieldingThread);
    }

    /* Lancement du thread gérant les différentes connexions client. */
    pthread_t connectionReceivingThread;
    if (pthread_create(&connectionReceivingThread, NULL, receivingMain, NULL) != 0) {
        printf("ERREUR : Le thread de monitoring des connexions n'a pas pu être créé.\n");
        endProgram(NUM_BUFFERS, EXIT_FAILURE);
    } else {
        pthread_detach(connectionReceivingThread);
    }
}
