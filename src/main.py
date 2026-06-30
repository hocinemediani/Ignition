import ctypes
import cv2
import time
import numpy as np

# Importation de la librairie partagée compilée depuis le code C.
lib = ctypes.CDLL("./libcamera.so")

# Création de la structure que l'on recevra en retour de l'appel à getLastFrame.
class PythonMessage(ctypes.Structure):
    _fields_ = [
        ("start", ctypes.c_void_p),
        ("size", ctypes.c_size_t),
        ("bufferIndex", ctypes.c_int)
    ]

# Configuration des différentes fonctions qui seront appelées.

## struct pythonMessage getLastFrame();
lib.getLastFrame.restype = PythonMessage

## void releaseLastFrame(int index);
lib.releaseLastFrame.argtypes = [ctypes.c_int]
lib.releaseLastFrame.restype = None

## void initializeCamera(int cameraWidth, int cameraHeight, int cameraIndex);
lib.initializeCamera.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
lib.initializeCamera.restype = None

# Initialisation de l'environnement.
lib.initializeCamera(640, 480, 0)
time.sleep(0.5)

# Définition de la fenêtre de prévisualisation.
windowName = "Flux Jetson Orin Nano"
cv2.namedWindow(windowName, cv2.WINDOW_NORMAL)

# Boucle de fonctionnement.
while (True):
    # Récupération et préparation de l'image.

    ## Récupération de la dernière image obtenue.
    message = lib.getLastFrame()

    ## Sécurité permettant de fermer le programme en appuyant sur q.
    if cv2.waitKey(1) & 0xFF == ord('q'):
        break

    ## Vérification de l'information récupérée.
    if message.start is None or message.size <= 0:
        print("En attente de la prochaine frame...")
        lib.releaseLastFrame(message.bufferIndex)
        continue
    
    ## Récupération du tableaux de pixels.
    ArrayType = ctypes.c_uint8 * message.size
    pixelsArray = ctypes.cast(message.start, ctypes.POINTER(ArrayType)).contents
    image = np.ctypeslib.as_array(pixelsArray)

    ## Si l'image n'a pas le bon format.
    if image.size != (640 * 480 * 2):
        lib.releaseLastFrame(message.bufferIndex)
        continue

    ## Conversion du tableau de pixels en image traitable par OpenCV.
    yuyvImage = image.reshape((480, 640, 2))
    bgrImage = cv2.cvtColor(yuyvImage, cv2.COLOR_YUV2BGR_YUYV)

    ## Affichage de l'image à l'écran.
    cv2.imshow(windowName, bgrImage)

    # Passage de l'image à travers le modèle YOLO.

    # Relachement du mutex associé au buffer.
    lib.releaseLastFrame(message.bufferIndex)

cv2.destroyAllWindows()
