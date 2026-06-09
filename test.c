#include "client.h"
#include <windows.h>

int main (int argc, char* argv[]) {

    /* Si une priorité et une taille maximale de matrice sont spécifiées. */
    if (argc > 2) {
        int matrixSize;
        if ((matrixSize = atoi(argv[1])) == 0) {
            printf("ERREUR : Veuillez saisir un entier pour la taille.\n");
            exit(EXIT_FAILURE);
        }

        int priority;
        if ((priority = atoi(argv[2])) == 0) {
            printf("ERREUR : Veuillez saisir un entier pour la priorité.\n");
            exit(EXIT_FAILURE);
        }

        printf("Simulation d'un flux continu de taille %d et de priorité %d.\n", matrixSize, priority);

        /* On simule un flux continu (et sporadique) de calculs envoyés. */
        while (1) {
            startMain(matrixSize, priority);
            srand(clock());
            Sleep(rand() / 10);
        }
        return 0;
    }

    /* Sinon, on fais des tests jusqu'a 2000x2000. */
    int maxMatrixSize = 2000;
    printf("Aucun argument n'a été récupéré, début des tests de benchmark.\n");

    /* Tests de benchmark ici. */
    for (int i = 1; i < 400; i += 10) {
        startMain(i, 0);
    }

    for (int i = 400; i < maxMatrixSize; i += 100) {
        startMain(i, 0);
    }
    return 0;
}