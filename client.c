/* Objectif, distribuer le calcul matriciel C = A x B entre les jetsons. */
#include <stdlib.h>
#include <stdio.h>

/** Méthode helper pour afficher une matrice à la console.
 * @param matrix La matrice a afficher
 * @param size La taille de la matrice
*/
void printMatrix(int* matrix, int size) {
    for (int k = 0; k < size * size; k++) {
        printf("%d ", matrix[k]);
        if (k != 0 && (k % size) == (size - 1)) {
            printf("\n");
        }
    }
}


/** Fonction principale permettant de :
 * - Vérifier les inputs,
 * 
*/
int main(int argc, char *argv[]) {
    /* 0. Vérification de l'input utilisateur. */
    if (argc < 2) {
        printf("ERREUR : Spécifiez une taille de matrice dans l'appel.\n");
        exit(EXIT_FAILURE);
    }

    int N;
    if ((N = atoi(argv[1])) == 0) {
        printf("ERREUR : Veuillez saisir un entier pour la taille.\n");
        exit(EXIT_FAILURE);
    }

    /* 1. Créer les deux matrices à calculer, A et B de taille NxN. */
    int *A = malloc(N * N * sizeof(int));
    int *B = malloc(N * N * sizeof(int));
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            A[i * N + j] = rand();
            B[i * N + j] = rand();
        }
    }

}
/* 2. Créer x sockets pour se connecter aux x jetson nano. */
/* 3. Envoyer en entier la matrice A et la matrice B par tranches. */
/* 4. Attendre la réception des résultats puis reconstruire la matrice C. */
/* 5. Vérifier qu'il n'y ait pas eu d'erreurs de calcul / race-conditions et mesurer le temps. */