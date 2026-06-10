#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <fcntl.h>

#define NUM_CARDS 6
#define PORT 8796
#define MAX_NUM_CONNECTION 15

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

/* Structure pour envoyer et récupérer les informations par le socket. */
typedef struct pthread_args {
    int index;
    int connectedIndex;
    void* result;
    char* ipAddress;
    int priority;
} pthread_args;

typedef struct monitoringMessage {
    char type[6];
    int sizeLeft;
} monitoringMessage;

/* Structure d'en-tête pour notifier de la taille des informations transitant. */
typedef struct messageHeader {
    int messageSize;
    int priority;
} messageHeader;

int sendMessage(socket_t clientSocket, const char *messageToSend, int size);

void* threadMain(void* _arg);

void setNonBlocking(socket_t clientSocket);

void checkConnectedJetsons(int* connectedCards);

void initializeAndStartThreads(pthread_args *args, int *connectedCards, pthread_t *threads);

int main();
