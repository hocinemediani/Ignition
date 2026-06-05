#include "client.h"

int main (int argc, char* argv[]) {
    /* 0. Vérification de l'input utilisateur. */
    if (argc < 2) {
        printf("ERREUR : Spécifiez une taille de matrice maximale dans l'appel.\n");
        exit(EXIT_FAILURE);
    }

    int maxMatrixSize = 2000;

    int numCards;
    if ((numCards = atoi(argv[1])) == 0) {
        printf("ERREUR : Veuillez saisir un entier pour le nombre de cartes connectées.\n");
        exit(EXIT_FAILURE);
    }

    for (int i = 1; i < 400; i++) {
        startMain(i, numCards);
    }

    for (int i = 400; i < maxMatrixSize; i += 100) {
        startMain(i, numCards);
    }
}