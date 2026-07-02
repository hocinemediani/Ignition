import tensorrt as trt
import pycuda.driver as cuda
import pycuda.autoinit
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

class CameraInfo(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_int),
        ("height", ctypes.c_int)
    ]

# Configuration des différentes fonctions qui seront appelées.

## void loadEngine();
lib.loadEngine.restype = None

## struct pythonMessage getLastFrame();
lib.getLastFrame.restype = PythonMessage

## void releaseLastFrame(int index);
lib.releaseLastFrame.argtypes = [ctypes.c_int]
lib.releaseLastFrame.restype = None

## void initializeCamera(int cameraWidth, int cameraHeight, int cameraIndex);
lib.initializeCamera.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
lib.initializeCamera.restype = None

## struct cameraInfo getCameraInfo();
lib.getCameraInfo.restype = CameraInfo

# Initialisation de l'environnement.
lib.initializeCamera(0, 0, 0)

camInfo = lib.getCameraInfo()
cameraWidth = camInfo.width
cameraHeight = camInfo.height 
print(cameraWidth, ":", cameraHeight)

time.sleep(0.5)

# Chargement (ou build) de l'engine.
lib.loadEngine()

# Définition de la fenêtre de prévisualisation.
windowName = "Flux Jetson Orin Nano"
cv2.namedWindow(windowName, cv2.WINDOW_NORMAL)

# Préparation du modèle.

## Load de l'engine et du contexte.
logger = trt.Logger(trt.Logger.ERROR)
runtime = trt.Runtime(logger)
with open("./models/yolov8n.engine", "rb") as engine:
    trtEngine = runtime.deserialize_cuda_engine(engine.read())
context = trtEngine.create_execution_context()

numTensors = trtEngine.num_io_tensors
buffers = []
memoryPointers = []

## Allocation des buffers d'entrée et de sortie.
for i in range (0, numTensors):
    ## Allocation du buffer host (en RAM).
    buffers.append(cuda.pagelocked_empty(tuple(trtEngine.get_tensor_shape(trtEngine.get_tensor_name(i))), np.float32))

    ## Allocation du buffer device (en VRAM).
    memoryPointers.append(cuda.mem_alloc(buffers[i].nbytes))
    
    ## Liaison (enregistrement) de l'adresse du buffer device dans le contexte TensorRT.
    context.set_tensor_address(trtEngine.get_tensor_name(i), int(memoryPointers[i]))

# Boucle de fonctionnement.
while (True):
    # Récupération de l'image.

    ## Récupération de la dernière image obtenue.
    message = lib.getLastFrame()

    ## Sécurité permettant de fermer le programme en appuyant sur q.
    if cv2.waitKey(1) & 0xFF == ord("q"):
        break

    ## Vérification de l'information récupérée.
    if message.start is None or message.size <= 0:
        print("En attente de la prochaine frame...")
        continue
    
    ## Récupération du tableaux de pixels.
    ArrayType = ctypes.c_uint8 * message.size
    pixelsArray = ctypes.cast(message.start, ctypes.POINTER(ArrayType)).contents
    rawImage = np.ctypeslib.as_array(pixelsArray)

    # Préparation de l'image.

    ## Récupération des caractéristiques de l'image.
    scale = min((640 / cameraWidth), (640 / cameraHeight))
    imageWidth = int(cameraWidth * scale)
    imageHeight = int(cameraHeight * scale)
    topPadding = (640 - imageHeight) // 2
    bottomPadding = (640 - imageHeight) // 2 + (640 - imageHeight) % 2
    leftPadding = (640 - imageWidth) // 2
    rightPadding = (640 - imageWidth) // 2 + (640 - imageWidth) % 2

    ## Transformation du tableau contigu de pixels en une image.
    yuyvImage = rawImage.reshape((cameraHeight, cameraWidth, 2))

    ## Conversion de l'image du format YUYV (natif) à RGB.
    rgbImage = cv2.cvtColor(yuyvImage, cv2.COLOR_YUV2RGB_YUYV)

    ## Redimensionnement de l'image.
    resizedImage = cv2.resize(rgbImage, (int(cameraWidth * scale), int(cameraHeight * scale)), cv2.INTER_LINEAR)

    ## Ajout des black-boxes pour convenir aux dimensions du tenseur d'entrée.
    fullRgbImage = cv2.copyMakeBorder(resizedImage, topPadding, bottomPadding, leftPadding, rightPadding, cv2.BORDER_CONSTANT, (0, 0, 0))

    ## Normalisation de la valeur des pixels.
    normalizedImage = (fullRgbImage / 255.0).astype(np.float32)

    ## Passage de HWC à CHW.
    chwImage = np.transpose(normalizedImage, (2, 0, 1))

    ## Ajout de la taille du batch pour correspondre aux dimensions du tenseur d'entrée (1, 3, 640, 640).
    correctImage = np.expand_dims(chwImage, axis=0)

    ## Aplatissement de l'image en un vecteur 1D.
    correctImage = np.ascontiguousarray(correctImage)
    image = np.ravel(correctImage)

    # Inférence.
    cudaStream = cuda.Stream()
    
    ## Copie de l'image dans le buffer d'entrée.
    np.copyto(buffers[0].ravel(), image)
    cuda.memcpy_htod_async(memoryPointers[0], buffers[0], cudaStream)

    ## Lancement de l'inférence.
    context.execute_async_v3(cudaStream.handle)

    ## Récupération du résultat.
    cuda.memcpy_dtoh_async(buffers[1], memoryPointers[1], cudaStream)

    ## Attente de la copie du résultat.
    cudaStream.synchronize()

    ## Tri des boîtes de détection.
    # A FAIRE

    ## Affichage de l'image à l'écran.
    cv2.imshow(windowName, cv2.cvtColor(fullRgbImage, cv2.COLOR_RGB2BGR))

    # Relachement du mutex associé au buffer.
    lib.releaseLastFrame(message.bufferIndex)

cv2.destroyAllWindows()
