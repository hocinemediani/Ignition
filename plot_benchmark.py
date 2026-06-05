# Ce script ne m'appartient pas, il à été entièrement généré par Gemini.
import pandas as pd
import matplotlib.pyplot as plt
import sys

# Nom du fichier CSV
fichier_csv = "resultatsGPU/Résultats finaux - GPU.csv"

try:
    # Lecture du fichier CSV
    df = pd.read_csv(fichier_csv)
except FileNotFoundError:
    print(f"ERREUR : Le fichier {fichier_csv} est introuvable dans le dossier actuel.")
    sys.exit(1)

# Affichage des colonnes disponibles
print("\n--- COLONNES DISPONIBLES ---")
colonnes = df.columns.tolist()
for i, col in enumerate(colonnes):
    print(f"[{i}] {col}")
print("----------------------------\n")

# Choix de l'axe X (généralement la Taille de Matrice, donc l'index 0)
try:
    x_idx_input = input("Entrez le numéro de la colonne pour l'axe X (ex: 0) : ")
    x_idx = int(x_idx_input) if x_idx_input.strip() else 0
    col_x = colonnes[x_idx]
except (ValueError, IndexError):
    print("Saisie invalide. Utilisation de la colonne 0 par défaut.")
    col_x = colonnes[0]

# Choix de l'axe Y (les différentes métriques de temps)
y_input = input("Entrez les numéros des colonnes pour l'axe Y, séparés par des virgules (ex: 1,7,8) : ")

try:
    # Transformation de l'entrée utilisateur en liste d'index
    y_idxs = [int(idx.strip()) for idx in y_input.split(',')]
    colonnes_y = [colonnes[idx] for idx in y_idxs]
except (ValueError, IndexError):
    print("ERREUR : Saisie invalide pour l'axe Y. Arrêt du script.")
    sys.exit(1)

# --- NOUVEAU : Sélection de la plage pour l'axe X ---
print(f"\n--- PLAGE DE VALEURS POUR L'AXE X ({col_x}) ---")
val_min_actuelle = df[col_x].min()
val_max_actuelle = df[col_x].max()
print(f"Valeurs actuelles dans le fichier : de {val_min_actuelle} à {val_max_actuelle}")

min_x_input = input("Entrez la valeur minimale (appuyez sur Entrée pour tout garder) : ")
max_x_input = input("Entrez la valeur maximale (appuyez sur Entrée pour tout garder) : ")

# Application des filtres sur le DataFrame
if min_x_input.strip():
    try:
        min_x = float(min_x_input)
        df = df[df[col_x] >= min_x]
    except ValueError:
        print("Saisie invalide pour le minimum. Ignoré.")

if max_x_input.strip():
    try:
        max_x = float(max_x_input)
        df = df[df[col_x] <= max_x]
    except ValueError:
        print("Saisie invalide pour le maximum. Ignoré.")

# Sécurité : vérifier s'il reste des données après le filtrage
if df.empty:
    print("ERREUR : La plage sélectionnée ne contient aucune donnée. Arrêt du script.")
    sys.exit(1)
# ----------------------------------------------------

# Configuration du graphique
plt.figure(figsize=(10, 6))

# Tracé de chaque courbe demandée
for col_y in colonnes_y:
    plt.plot(df[col_x], df[col_y], marker='o', markersize=3, linestyle='-', label=col_y)

# Personnalisation du graphique
plt.xlabel(col_x)
plt.ylabel("Temps (secondes)")
plt.title(f"Évolution des temps de calcul en fonction de : {col_x}")
plt.legend()
plt.grid(True, linestyle='--', alpha=0.7)

# Affichage de la fenêtre interactive
plt.tight_layout()
plt.show()