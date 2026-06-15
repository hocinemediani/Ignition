#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>

#define NUM_CARDS 6
#define PORT 8796
#define MAX_NUM_CONNECTION 15
#define MAX_CONCURRENT_TASKS 25

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
    int priority;
    socket_t socket;
} pthread_args;

/* Structure pour récupérer des informations utiles depuis les workers. */
typedef struct monitoringMessage {
    /* Possible types : HELLO, BYE, INFO. */
    char type[6];
    int sizeLeft;
    int workerIndex;
} monitoringMessage;

/* Structure d'en-tête pour notifier de la taille des informations transitant. */
typedef struct messageHeader {
    int messageSize;
    int priority;
    int taskID;
} messageHeader;

void sigIntHandler(int sig);

void initInteruptHandling();

int receiveMessage(socket_t clientSocketFd, void *messageToReceive, int size);

int sendMessage(socket_t clientSocket, const char *messageToSend, int size);

void setNonBlocking(socket_t clientSocket);

void checkConnectedJetsons();

void initializeSocket(socket_t *orchestratorSocket);

void* clientListening(void* _arg);

void handleConnection(socket_t clientSocket, int index);

void getInformationFromWorker(socket_t workerSocket, int workerIndex);

void* listenToWorkers(void *);

void checkForConnections(fd_set *fdSet, socket_t *mainSocket, socket_t *socketTable, int tableSize, int read, void handler(socket_t, int));

void* workerListening(void* _arg);

int main();
