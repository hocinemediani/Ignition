#include "camera.h"

/* Structure servant à communiquer avec le script python. */
struct pythonMessage {
    void *start;
    size_t size;
    int bufferIndex;
} pythonMessage;

/* Structure conservant l'adresse du début du tableau de pixel et la taille maximale du buffer vidéo. */
struct videoBuffer {
    void *start;
    size_t length;
} videoBuffer;

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


void endProgram(int toClean, int exitCode) {
    for (int i = 0; i < toClean; i++) {
        munmap(videoBuffers[i].start, videoBuffers[i].length);
    }
    exit(exitCode);
}


int filterFileName(const struct dirent *file) {
    if (strncmp(file->d_name, "video", 5) == 0) return 1;
    return 0;
}


void releaseLastFrame(int index) {
    struct v4l2_buffer buffer;
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    ioctl(desiredCameraFd, VIDIOC_QBUF, &buffer);
    pthread_mutex_unlock(&bufferMutexes[index]);
}


struct pythonMessage getLastFrame() {
    /* Tentative de récupération du buffer ayant la denrière image. */
    pthread_mutex_lock(&messageMutex);
    
    /* On marque ce buffer comme utilisé. */
    pthread_mutex_lock(&bufferMutexes[message.bufferIndex]);

    /* Copie du message à envoyer étant donné qu'on ne pourra pas unlock après le return. */
    struct pythonMessage messageToSend = message;
    pthread_mutex_unlock(&messageMutex);

    return messageToSend;
}


void *threadMain(void *_arg) {
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


void initializeCamera(int cameraWidth, int cameraHeight, int cameraIndex) {
    /* Récupérer les différentes caméras disponibles. */
    struct dirent **namelist;
    int scanReturn = scandir("/dev/", &namelist, filterFileName, alphasort);
    if (scanReturn <= 0) {
        printf("ERREUR : Il y a eu un problème lors de la récupération des caméras.\n");
        exit(EXIT_FAILURE);
    }

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

    printf("Format utilisé par la caméra : %d:%d\n", videoFormat.fmt.pix.width, videoFormat.fmt.pix.height);

    /* Configuration des paramètres du flux vidéo. */
    struct v4l2_requestbuffers reqBuffers = {0};
    reqBuffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqBuffers.memory = V4L2_MEMORY_MMAP;
    reqBuffers.count = NUM_BUFFERS;
    if (ioctl(desiredCameraFd, VIDIOC_REQBUFS, &reqBuffers) == -1) {
        printf("ERREUR : Impossible d'allouer les buffers vidéo.\n");
        exit(EXIT_FAILURE);
    }

    /* Récupération et stockage des addresses de mémoire partagée avec la caméra. */
    for (int i = 0; i < NUM_BUFFERS; i++) {
        struct v4l2_buffer videoBuffer;
        videoBuffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        videoBuffer.memory = V4L2_MEMORY_MMAP;
        videoBuffer.index = i;
        if (ioctl(desiredCameraFd, VIDIOC_QUERYBUF, &videoBuffer) == -1) {
            printf("ERREUR : Le buffer vidéo %d n'a pas pu être configuré.\n", i);
            endProgram(i, EXIT_FAILURE);
        }

        videoBuffers[i].length = videoBuffer.length;
        videoBuffers[i].start = mmap(NULL, videoBuffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, desiredCameraFd, videoBuffer.m.offset);

        if (videoBuffers[i].start == MAP_FAILED) {
            printf("ERREUR : Impossible de récupérer l'adresse de la mémoire partagée pour le buffer %d.\n", i);
            endProgram(i, EXIT_FAILURE);
        }
        ioctl(desiredCameraFd, VIDIOC_QBUF, &videoBuffer);
    }

    /* Initilisation des mutexes pour les buffers. */
    pthread_mutex_init(&messageMutex, NULL);
    for (int i = 0; i < NUM_BUFFERS; i++) {
        pthread_mutex_init(&bufferMutexes[i], NULL);
    }

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
}
