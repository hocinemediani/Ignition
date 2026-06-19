#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <libgen.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#define MAX_FILEPATH_LENGTH 63
#define ORCHESTRATOR_PORT 8796

/* Instructions préprocesseur pour différencier les architectures linux de windows qui ont des imports différents. */
#ifdef _WIN32
    #include <winsock2.h>
    #include <process.h>
    #include <io.h>

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

/* Structure d'en-tête pour notifier de la taille des informations transitant. */
typedef struct messageHeader {
    int messageSize;
    int priority;
    int taskID;
    char model[64];
    /* 0 -> Utilisation d'un modèle existant,
    *  1 -> Soumission d'un nouveau modèle,
    *  2 -> Erreur de compilation/exécution worker,
    *  3 -> Aucune carte n'est disponible pour prendre la tâche,
    *  4 -> Modèle non existant. */
    int action;
} messageHeader;

int sendMessage(socket_t clientSocket, const char *messageToSend, int size);

void readFromFile(int fileSize, int cudaFileFd, char *fileString, char *filePath);

int getFileSize(char *filePath, int *cudaFileFd);

int receiveMessage(int clientSocketFd, void *messageToReceive, int size);

void connectToOrchestrator(socket_t *clientSocket);

void verifyUserInput(int argc, char *argv[]);
