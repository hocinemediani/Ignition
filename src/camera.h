#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>

#define NUM_BUFFERS 4

void endProgram(int toClean, int exitCode);

int filterFileName(const struct dirent *file);

void releaseLastFrame(int index);

struct pythonMessage getLastFrame();

void *threadMain(void *_arg);

void initializeCamera(int cameraWidth, int cameraHeight, int cameraIndex);
