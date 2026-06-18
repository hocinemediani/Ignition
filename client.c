#include "client.h"

int regularRequest = 1;

/** Méthode helper afin d'envoyer un bloc d'information à la carte worker.
 * @param clientSocket Le socket depuis lequel envoyer les informations
 * @param messageToSend Le message à envoyer
 * @param size La taille du message à envoyer
 * @return -1 si l'envoi à échoué, 0 si l'envoi s'est bien déroulé
 */
int sendMessage(socket_t clientSocket, const char *messageToSend, int size) {
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


/** Méthode helper afin de lire un fichier situé à filePath dans la variable fileString.
 * @param fileSize La taille du fichier à lire
 * @param fileToSendFd Le descripteur de fichier pointant sur le fichier à lire
 * @param fileString Le buffer à remplir du contenu du fichier lu
 * @param filePath Le chemin vers le fichier, utilisé en cas d'erreur
 */
void readFromFile(int fileSize, int fileToSendFd, char *fileString, char *filePath) {
    int readBytes = 0;
    int bytesToRead = fileSize;
    while (readBytes < fileSize) {
        int bytesRead = 0;
        if ((bytesRead = read(fileToSendFd, fileString + readBytes, bytesToRead)) == -1) {
            printf("ERREUR : La lecture du fichier %s s'est mal déroulée.\n", filePath);
            exit(EXIT_FAILURE);
        }

        if (bytesRead == 0) {
            break;
        }
        readBytes += bytesRead;
        bytesToRead -= bytesRead;
    }
}


/** Méthode helper afin d'obtenir la taille et un descripteur de fichier vers le fichier situé à filePath.
 * @param filePath Le chemin auquel le fichier se trouve
 * @param fileToSendFd Le descripteur de fichier sur filePath
 * @return La taille du fichier situé à filePath
 */
int getFileSize(char *filePath, int *fileToSendFd) {
    if ((*fileToSendFd = open(filePath, O_RDONLY | O_BINARY)) == -1) {
        printf("ERREUR : Impossible d'ouvrir le fichier : %s", filePath);
        exit(EXIT_FAILURE);
    }

    int fileSize = 0;
    struct stat fileStats;
    if (fstat(*fileToSendFd, &fileStats) == -1) {
        printf("ERREUR : L'appel à fstat à échoué.\n");
        exit(EXIT_FAILURE);
    }
    return fileSize = fileStats.st_size;
}


/** Méthode helper afin de recevoir un bloc d'information depuis le réseau.
 * @param clientSocketFd Le file descriptor du socket vers lequel sont envoyées les informations
 * @param messageToReceive Le message à recevoir du réseau
 * @param size La taille du message à recevoir
 * @return -1 si l'envoi à échoué, 0 si l'envoi s'est bien déroulé
 */
int receiveMessage(int clientSocketFd, void *messageToReceive, int size) {
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


/** Méthode helper afin de se connecter à l'orchestrateur.
 * @param clientSocket Le socket client à modifier et depuis lequel se connecter
 */
void connectToOrchestrator(socket_t *clientSocket) {
    if ((*clientSocket = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
        printf("ERREUR : La création du socket n'a pas aboutie.\n");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in orchestratorAddress;
    memset(&orchestratorAddress, 0, sizeof(orchestratorAddress));

    orchestratorAddress.sin_addr.s_addr = inet_addr("147.127.121.93");
    orchestratorAddress.sin_family = AF_INET;
    orchestratorAddress.sin_port = htons(ORCHESTRATOR_PORT);

    if (connect(*clientSocket, (const struct sockaddr *) &orchestratorAddress, sizeof(orchestratorAddress)) == -1) {
        printf("ERREUR : La connexion à l'orchestrateur à échouée.\n");
        exit(EXIT_FAILURE);
    }
    printf("Connexion réussie à l'orchestrateur.\n");
}


/** Méthode helper afin de vérifier la conformité de l'input donné par l'utilisateur.
 * @param argc Le nombre d'arguments donné par l'utilisateur
 * @param argv La liste des arguments donnée par l'utilisateur
 */
void verifyUserInput(int argc, char *argv[]) {
    if (argc > 4 || argc < 3) {
        printf("ERREUR : Vous pouvez utiliser le programme de deux façons :\n\t1. client nomDeFichier modeleVoulu priorite\n\t2. client fichierONNX fichierInference\n");
        exit(EXIT_FAILURE);
    }

    /* Si on souhaite utiliser les modèles présents. */
    if (argc == 4) {
        if (strlen(argv[1]) > MAX_FILEPATH_LENGTH) {
            printf("ERREUR : Le nom de fichier donné est trop long.\n");
            exit(EXIT_FAILURE);
        }

        if (strlen(argv[2]) > MAX_FILEPATH_LENGTH) {
            printf("ERREUR : Le nom de modèle donné est trop long.\n");
            exit(EXIT_FAILURE);
        }
        
        if (atoi(argv[3]) == 0) {
            printf("Le troisième argument doit être la priorité du flux envoyé.\n");
            exit(EXIT_FAILURE);
        }
    }

    /* Si on souhaite soumettre un nouveau modèle. */
    if (argc == 3) {
        char *lastDotPosition;
        if (((lastDotPosition = strrchr(argv[1], '.')) == NULL) || (strcmp(lastDotPosition + 1, "onnx") != 0)) {
            printf("ERREUR : Veuillez spécifier un fichier de modèle conforme, du type .onnx.\n");
            exit(EXIT_FAILURE);
        }

        if (((lastDotPosition = strrchr(argv[2], '.')) == NULL) || (strcmp(lastDotPosition + 1, "cpp") != 0)) {
            printf("ERREUR : Veuillez spécifier un fichier d'inférence conforme, du type .cpp.\n");
            exit(EXIT_FAILURE);
        }
        regularRequest = 0;
    }

}


/** Fonction principale du client, permettant de :
 * - Vérifier la bonne extension du fichier à soumettre.
 * - Créer un socket se connectant à l'orchestrateur.
 * - Soumettre à l'orchestrateur un fichier pour traitement par les workers.
 * - Récupérer les résultats depuis l'orchestrateur et les afficher à la console.
 */
int main (int argc, char *argv[]) {
    /* 0. Vérification de l'input utilisateur. */
    verifyUserInput(argc, argv);

    /* 1. Création d'un socket qui se connecte à l'orchestrateur. */
    init_connection();
    socket_t clientSocket;
    
    connectToOrchestrator(&clientSocket);

    if (regularRequest == 1) {
        /* Si on souhaite utiliser les modèles présents. */
        char filePath[MAX_FILEPATH_LENGTH];
        sprintf(filePath, "%s", argv[1]);

        int fileToSendFd;
        int fileSize = getFileSize(filePath, &fileToSendFd);

        char *fileString = malloc((fileSize + 1));
        memset(fileString, 0, (fileSize + 1));
        
        readFromFile(fileSize, fileToSendFd, fileString, filePath);

        /* Envoyer le header avec le bon modèle, la priorité etc. */
        struct messageHeader header;
        header.messageSize = fileSize;
        header.priority = atoi(argv[3]);
        header.taskID = 0;
        header.action = 0;
        sprintf(header.model, "%s", argv[2]);

        if (sendMessage(clientSocket, (const char *) &header, sizeof(header)) == -1) {
            printf("ERREUR : L'envoi du header s'est mal déroulé.\n");
            goto cleanup;
        }

        /* Envoyer le fichier à traiter. */
        if (sendMessage(clientSocket, fileString, fileSize) == -1) {
            printf("ERREUR : L'envoi du fichier à traiter n'a pas pu aboutir.\n");
            goto cleanup;
        }

        cleanup:
        close(fileToSendFd);
        free(fileString);
    } else {
        /* Si on souhaite ajouter un nouveau modèle. */
        int onnxFileFd;
        int inferenceFileFd;

        int onnxFileSize = getFileSize(argv[1], &onnxFileFd);
        int inferenceFileSize = getFileSize(argv[2], &inferenceFileFd);

        char *onnxFileString = malloc(onnxFileSize);
        readFromFile(onnxFileSize, onnxFileFd, onnxFileString, argv[1]);
        char *inferenceFileString = malloc(inferenceFileSize);
        readFromFile(inferenceFileSize, inferenceFileFd, inferenceFileString, argv[2]);

        char *fileName = basename(argv[1]);
        char *lastDotPosition;
        lastDotPosition = strrchr(fileName, '.');

        char *modelName = malloc(lastDotPosition - fileName + 1);
        snprintf(modelName, lastDotPosition - fileName + 1, "%s", fileName);
        
        /* Envoyer le header avec le bon modèle, la priorité etc. */
        struct messageHeader header;
        header.messageSize = onnxFileSize;
        header.priority = 0;
        header.taskID = 0;
        header.action = 1;
        sprintf(header.model, "%s", modelName);

        if (sendMessage(clientSocket, (const char *) &header, sizeof(header)) == -1) {
            printf("ERREUR : L'envoi du premier header s'est mal déroulé.\n");
            goto cleanup;
        }

        /* Envoi du fichier .onnx. */
        if (sendMessage(clientSocket, onnxFileString, onnxFileSize) == -1) {
            printf("ERREUR : L'envoi du fichier .onnx s'est mal déroulé.\n");
            goto cleanup;
        }

        /* Envoi du header pour la reception du fichier d'inférence. */
        header.messageSize = inferenceFileSize;
        header.priority = 0;
        header.taskID = 0;
        header.action = 1;
        sprintf(header.model, "%s", modelName);

        if (sendMessage(clientSocket, (const char *) &header, sizeof(header)) == -1) {
            printf("ERREUR : L'envoi du premier header s'est mal déroulé.\n");
            goto cleanup;
        }

        /* Envoi du fichier d'inférence. */
        if (sendMessage(clientSocket, inferenceFileString, inferenceFileSize) == -1) {
            printf("ERREUR : L'envoi du fichier d'inférence s'est mal déroulé.\n");
            goto cleanup;
        }

        printf("En attente de la confirmation de compilation de la worker, cela peut prendre jusqu'a 2 minutes.\n");
    }

    /* 3. Attente de réception des résultats depuis l'orchestrateur. */
    struct messageHeader header;
    memset(&header, 0, sizeof(header));

    if (receiveMessage(clientSocket, &header, sizeof(header)) == -1) {
        printf("ERREUR : La réception du header s'est mal déroulée.\n");
        exit(EXIT_FAILURE);
    }

    if (header.action == 1) {
        printf("Compilation réalisée avec succès, le modèle %s est maintenant disponible sur les workers !\n", header.model);
        exit(EXIT_SUCCESS);
    } else if (header.action == 2) {
        /* Si il y a eu une erreur de compilation ou d'exécution. */
        printf("ERREUR : La tâche n'a pas pu être traitée par la worker.\n");
        exit(EXIT_FAILURE);
    } else if (header.action == 3) {
        /* Si il n'y a aucune carte pouvant accepter de nouvelle tâche. */
        printf("ERREUR : Aucune carte ne peut accepter de tâche pour le moment.\n");
        exit(EXIT_FAILURE);
    } else if (header.action == 4) {
        /* Si le modèle n'existe pas. */
        char *availableModels = malloc(header.messageSize);
        if (receiveMessage(clientSocket, availableModels, header.messageSize) == -1) {
            printf("ERREUR : Le message d'information de l'orchestrateur n'a pas pu être reçu.\n");
            exit(EXIT_FAILURE);
        }
        printf("ERREUR : Veuillez choisir parmis les modèles suivants :\n");
        
        int i = 0;
        while (i < (int) strlen(availableModels)) {
            printf("\t- ");
            while (availableModels[i] != '\n' && availableModels[i] != '\0') {
                printf("%c", availableModels[i]);
                i++;
            }
            printf("\n");
            i++;
        }
        exit(EXIT_FAILURE);
    }

    /* Récupération des résultats. */
    char *results = malloc(header.messageSize + 1);
    if (receiveMessage(clientSocket, results, header.messageSize) == -1) {
        printf("ERREUR : La réception des résultats s'est mal déroulée.\n");
        free(results);
        exit(EXIT_FAILURE);
    }

    results[header.messageSize] = '\0';
    printf("Résultats reçus :\n\n%s", results);
    return 0;
}