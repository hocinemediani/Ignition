#include "client.h"

int main (int argc, char* argv[]) {

    if (argc > 1) {
        int matrixSize;

        if ((matrixSize = atoi(argv[1])) == 0) {
            printf("ERREUR : Veuillez saisir un entier pour la taille.\n");
            exit(EXIT_FAILURE);
        }

        startMain(matrixSize);
        return 0;
    }

    int maxMatrixSize = 2000;

    for (int i = 1; i < 400; i++) {
        startMain(i);
    }

    for (int i = 400; i < maxMatrixSize; i += 100) {
        startMain(i);
    }
    return 0;
}