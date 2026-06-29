#include "camera.h"

int filterFileName(const struct dirent *file) {
    if (strncmp(file->d_name, "video", 5) == 0) return 1;
    return 0;
}


int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("ERREUR : Veuillez renseigner une largeur d'image, haute d'image et l'index de la caméra souhaitée (0 si inconnu).\n");
        exit(EXIT_FAILURE);
    }

    int cameraWidth;
    if ((cameraWidth = atoi(argv[1])) == 0) {
        printf("ERREUR : Impossible de récupérer la largeur de l'image souhaitée : %s.\n", argv[1]);
        exit(EXIT_FAILURE);
    }

    int cameraHeight;
    if ((cameraHeight = atoi(argv[2])) == 0) {
        printf("ERREUR : Impossible de récupérer la hauteur de l'image souhaitée : %s.\n", argv[2]);
        exit(EXIT_FAILURE);
    }

    int cameraIndex = atoi(argv[3]);
    struct dirent **namelist;

    /* Récupérer les différentes caméras disponibles. */
    int scanReturn = scandir("/dev/", &namelist, filterFileName, alphasort);
    if (scanReturn <= 0) {
        printf("ERREUR : Il y a eu un problème lors de la récupération des caméras.\n");
        exit(EXIT_FAILURE);
    }

    char filePath[266];
    int numCameras = 0;
    int desiredCameraFd = -1;
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

    /* Récupérer le tableau de pixels correspondant à l'image. */
    
    struct v4l2_requestbuffers reqBuffers = {0};
    reqBuffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqBuffers.memory = V4L2_MEMORY_MMAP;
    reqBuffers.count = 4;
    if (ioctl(desiredCameraFd, VIDIOC_REQBUFS, &reqBuffers) == -1) {
        printf("ERREUR : Impossible d'allouer les buffers vidéo.\n");
        exit(EXIT_FAILURE);
    }
    
    return 0;
}
