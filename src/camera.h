#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define NUM_BUFFERS 4
#define MAX_CONCURRENT_CONNECTIONS 5

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

/* Structure contenant les informations utiles de la caméra (sa hauteur et sa largeur). */
struct cameraInfo {
    int width;
    int height;
} cameraInfo;

void endProgram(int toClean, int exitCode);

struct cameraInfo getCameraInfo();

void loadEngine(const char *enginePath, int needsRebuild);

int filterFileName(const struct dirent *file);

void releaseLastFrame(int index);

struct pythonMessage getLastFrame();

void *threadMain(void *_arg);

void* receivingMain(void *_arg);

void sendDetections(void *detectionList, int size);

void sendImage(void *image, uint32_t imageSize);

void initializeCamera(int cameraWidth, int cameraHeight, int cameraIndex);
